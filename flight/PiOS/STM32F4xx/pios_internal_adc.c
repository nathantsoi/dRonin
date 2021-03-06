/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup   PIOS_ADC ADC Functions
 * @{
 *
 * @file       pios_internal_adc.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @author     Michael Smith Copyright (C) 2011.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2012-2014
 * @author     dRonin, http://dronin.org Copyright (C) 2015
 * @brief      STM32F4xx Internal ADC PIOS interface
 * @see        The GNU Public License (GPL) Version 3
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * @note This is a stripped-down ADC driver intended primarily for sampling
 * voltage and current values.  Samples are averaged over the period between
 * fetches so that relatively accurate measurements can be obtained without
 * forcing higher-level logic to poll aggressively.
 *
 * @todo This module needs more work to be more generally useful.  It should
 * almost certainly grow callback support so that e.g. voltage and current readings
 * can be shipped out for coulomb counting purposes.  The F1xx interface presumes
 * use with analog sensors, but that implementation largely dominates the ADC
 * resources.  Rather than commit to a new API without a defined use case, we
 * should stick to our lightweight subset until we have a better idea of what's needed.
 */

#include "pios.h"
#include <pios_internal_adc_priv.h>

#if defined(PIOS_INCLUDE_ADC)

#include "pios_queue.h"

// Private types
enum pios_adc_dev_magic {
	PIOS_INTERNAL_ADC_DEV_MAGIC = 0x58375124,
};

struct pios_internal_adc_dev {
	const struct pios_internal_adc_cfg * cfg;
	ADCCallback callback_function;
#if defined(PIOS_INCLUDE_CHIBIOS)
	struct pios_queue *data_queue;
#endif
	volatile int16_t *valid_data_buffer;
	volatile uint8_t adc_oversample;
	uint8_t dma_block_size;
	uint16_t dma_half_buffer_size;
	uint16_t max_samples;
	enum pios_adc_dev_magic magic;
};

static struct pios_internal_adc_dev * pios_adc_dev;

// Private functions
static struct pios_internal_adc_dev * PIOS_INTERNAL_ADC_Allocate(const struct pios_internal_adc_cfg * cfg);
static bool PIOS_INTERNAL_ADC_validate(struct pios_internal_adc_dev *);

#if defined(PIOS_INCLUDE_ADC)
static void init_pins(void);
static void init_dma(void);
static void init_adc(void);
#endif
static int32_t PIOS_INTERNAL_ADC_PinGet(uint32_t internal_adc_id, uint32_t pin);
static uint8_t PIOS_INTERNAL_ADC_Number_of_Channels(uint32_t internal_adc_id);
static bool PIOS_INTERNAL_ADC_Available(uint32_t adc_id, uint32_t device_pin);
static float PIOS_INTERNAL_ADC_LSB_Voltage(uint32_t internal_adc_id);

const struct pios_adc_driver pios_internal_adc_driver = {
                .available      = PIOS_INTERNAL_ADC_Available,
                .get_pin        = PIOS_INTERNAL_ADC_PinGet,
                .set_queue      = NULL,
                .number_of_channels = PIOS_INTERNAL_ADC_Number_of_Channels,
                .lsb_voltage = PIOS_INTERNAL_ADC_LSB_Voltage,
};

struct adc_accumulator {
	uint32_t		accumulator;
	uint32_t		count;
};

// Buffers to hold the ADC data
static struct adc_accumulator * accumulator;
static uint16_t * adc_raw_buffer_0;
static uint16_t * adc_raw_buffer_1;


static void init_pins(void)
{
	if (!PIOS_INTERNAL_ADC_validate(pios_adc_dev)) {
		return;
	}

	/* Setup analog pins */
	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_StructInit(&GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Speed	= GPIO_Speed_2MHz;
	GPIO_InitStructure.GPIO_Mode	= GPIO_Mode_AIN;
	
	for (int32_t i = 0; i < pios_adc_dev->cfg->adc_pin_count; i++) {
		if (pios_adc_dev->cfg->adc_pins[i].port == NULL)
			continue;
		GPIO_InitStructure.GPIO_Pin = pios_adc_dev->cfg->adc_pins[i].pin;
		GPIO_Init(pios_adc_dev->cfg->adc_pins[i].port, &GPIO_InitStructure);
	}
}

static void init_dma(void)
{
	if (!PIOS_INTERNAL_ADC_validate(pios_adc_dev)) {
		return;
	}

	/* Disable interrupts */
	DMA_ITConfig(pios_adc_dev->cfg->dma.rx.channel, pios_adc_dev->cfg->dma.irq.flags, DISABLE);

	/* Configure DMA channel */
	DMA_DeInit(pios_adc_dev->cfg->dma.rx.channel);
	DMA_InitTypeDef DMAInit = pios_adc_dev->cfg->dma.rx.init;
	DMAInit.DMA_Memory0BaseAddr		= (uint32_t)&adc_raw_buffer_0[0];
	DMAInit.DMA_BufferSize			= pios_adc_dev->max_samples * pios_adc_dev->cfg->adc_pin_count;
	DMAInit.DMA_DIR					= DMA_DIR_PeripheralToMemory;
	DMAInit.DMA_PeripheralInc		= DMA_PeripheralInc_Disable;
	DMAInit.DMA_MemoryInc			= DMA_MemoryInc_Enable;
	DMAInit.DMA_PeripheralDataSize	= DMA_PeripheralDataSize_HalfWord;
	DMAInit.DMA_MemoryDataSize		= DMA_MemoryDataSize_HalfWord;
	DMAInit.DMA_Mode				= DMA_Mode_Circular;
	DMAInit.DMA_Priority			= DMA_Priority_Low;
	DMAInit.DMA_FIFOMode			= DMA_FIFOMode_Disable;
	DMAInit.DMA_FIFOThreshold		= DMA_FIFOThreshold_HalfFull;
	DMAInit.DMA_MemoryBurst			= DMA_MemoryBurst_Single;
	DMAInit.DMA_PeripheralBurst		= DMA_PeripheralBurst_Single;

	DMA_Init(pios_adc_dev->cfg->dma.rx.channel, &DMAInit);	/* channel is actually stream ... */

	/* configure for double-buffered mode and interrupt on every buffer flip */
	DMA_DoubleBufferModeConfig(pios_adc_dev->cfg->dma.rx.channel, (uint32_t)&adc_raw_buffer_1[0], DMA_Memory_0);
	DMA_DoubleBufferModeCmd(pios_adc_dev->cfg->dma.rx.channel, ENABLE);
	DMA_ITConfig(pios_adc_dev->cfg->dma.rx.channel, DMA_IT_TC, ENABLE);
	//DMA_ITConfig(pios_adc_dev->cfg->dma.rx.channel, DMA_IT_HT, ENABLE);

	/* enable DMA */
	DMA_Cmd(pios_adc_dev->cfg->dma.rx.channel, ENABLE);

	/* Configure DMA interrupt */
	NVIC_InitTypeDef NVICInit = pios_adc_dev->cfg->dma.irq.init;
	NVIC_Init(&NVICInit);
}

static void init_adc(void)
{
	if (!PIOS_INTERNAL_ADC_validate(pios_adc_dev)) {
		return;
	}

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

	ADC_DeInit();

	/* turn on VREFInt in case we need it */
	ADC_TempSensorVrefintCmd(ENABLE);

	/* Do common ADC init */
	ADC_CommonInitTypeDef ADC_CommonInitStructure;
	ADC_CommonStructInit(&ADC_CommonInitStructure);
	ADC_CommonInitStructure.ADC_Mode				= ADC_Mode_Independent;
	ADC_CommonInitStructure.ADC_Prescaler			= ADC_Prescaler_Div8;
	ADC_CommonInitStructure.ADC_DMAAccessMode		= ADC_DMAAccessMode_Disabled;
	ADC_CommonInitStructure.ADC_TwoSamplingDelay	= ADC_TwoSamplingDelay_5Cycles;
	ADC_CommonInit(&ADC_CommonInitStructure);

	ADC_InitTypeDef ADC_InitStructure;
	ADC_StructInit(&ADC_InitStructure);
	ADC_InitStructure.ADC_Resolution				= ADC_Resolution_12b;
	ADC_InitStructure.ADC_ScanConvMode				= ENABLE;
	ADC_InitStructure.ADC_ContinuousConvMode		= ENABLE;
	ADC_InitStructure.ADC_ExternalTrigConvEdge		= ADC_ExternalTrigConvEdge_None;
	ADC_InitStructure.ADC_DataAlign					= ADC_DataAlign_Right;
	ADC_InitStructure.ADC_NbrOfConversion			= pios_adc_dev->cfg->adc_pin_count;
	ADC_Init(pios_adc_dev->cfg->adc_dev_master, &ADC_InitStructure);

	/* Enable DMA request */
	ADC_DMACmd(pios_adc_dev->cfg->adc_dev_master, ENABLE);

	/* Configure input scan */
	for (int32_t i = 0; i < pios_adc_dev->cfg->adc_pin_count; i++) {
		ADC_RegularChannelConfig(pios_adc_dev->cfg->adc_dev_master,
				pios_adc_dev->cfg->adc_pins[i].adc_channel,
				i+1,
				ADC_SampleTime_56Cycles);		/* XXX this is totally arbitrary... */
	}

	ADC_DMARequestAfterLastTransferCmd(pios_adc_dev->cfg->adc_dev_master, ENABLE);

	/* Finally start initial conversion */
	ADC_Cmd(pios_adc_dev->cfg->adc_dev_master, ENABLE);
	ADC_ContinuousModeCmd(pios_adc_dev->cfg->adc_dev_master, ENABLE);
	ADC_SoftwareStartConv(pios_adc_dev->cfg->adc_dev_master);
}

static bool PIOS_INTERNAL_ADC_validate(struct pios_internal_adc_dev * dev)
{
	if (dev == NULL)
		return false;
	
	return (dev->magic == PIOS_INTERNAL_ADC_DEV_MAGIC);
}

static struct pios_internal_adc_dev * PIOS_INTERNAL_ADC_Allocate(const struct pios_internal_adc_cfg * cfg)
{
	struct pios_internal_adc_dev * adc_dev;

	adc_dev = (struct pios_internal_adc_dev *)PIOS_malloc(sizeof(*adc_dev));
	if (!adc_dev) {
		return NULL;
	}

	// Maxmimum number of samples (XXX: not sure where the dependency on the ADC used comes from..)
	bool use_adc_2 = cfg->adc_dev_master == ADC2;
	adc_dev->max_samples = (((cfg->adc_pin_count + use_adc_2) >> use_adc_2) << use_adc_2) * PIOS_ADC_MAX_OVERSAMPLING * 2;

	accumulator = (struct adc_accumulator *)PIOS_malloc_no_dma(cfg->adc_pin_count * sizeof(struct adc_accumulator));
	if (!accumulator) {
		PIOS_free(adc_dev);
		return NULL;
	}

	adc_raw_buffer_0 = (uint16_t *)PIOS_malloc(adc_dev->max_samples * cfg->adc_pin_count * sizeof(uint16_t));
	if (!adc_raw_buffer_0) {
		PIOS_free(adc_dev);
		PIOS_free(accumulator);
		return NULL;
	}

	adc_raw_buffer_1 = (uint16_t *)PIOS_malloc(adc_dev->max_samples * cfg->adc_pin_count * sizeof(uint16_t));
	if (!adc_raw_buffer_1) {
		PIOS_free(adc_dev);
		PIOS_free(accumulator);
		PIOS_free(adc_raw_buffer_0);
		return NULL;
	}

	adc_dev->magic = PIOS_INTERNAL_ADC_DEV_MAGIC;
	return(adc_dev);
}

/**
 * @brief Init the ADC.
 */
int32_t PIOS_INTERNAL_ADC_Init(uint32_t * internal_adc_id, const struct pios_internal_adc_cfg * cfg)
{
	pios_adc_dev = PIOS_INTERNAL_ADC_Allocate(cfg);
	if (pios_adc_dev == NULL)
		return -1;
	
	pios_adc_dev->cfg = cfg;
	pios_adc_dev->callback_function = NULL;



#if defined(PIOS_INCLUDE_CHIBIOS)
	pios_adc_dev->data_queue = NULL;
#endif

#if defined(PIOS_INCLUDE_ADC)
	init_pins();
	init_dma();
	init_adc();
#endif
	*internal_adc_id = (uint32_t)pios_adc_dev;
	return 0;
}

/**
 * @brief Configure the ADC to run at a fixed oversampling
 * @param[in] oversampling the amount of oversampling to run at
 */
void PIOS_ADC_Config(uint32_t oversampling)
{
	/* we ignore this */
}

/**
 * Returns value of an ADC Pin
 * @param[in] pin number
 * @return ADC pin value averaged over the set of samples since the last reading.
 * @return -1 if pin doesn't exist
 * @return -2 if no data acquired since last read
 * TODO we currently ignore internal_adc_id since this driver doesn't support multiple instances
 * TODO we should probably refactor this similarly to the new F3 driver
 */
static int32_t PIOS_INTERNAL_ADC_PinGet(uint32_t internal_adc_id, uint32_t pin)
{
#if defined(PIOS_INCLUDE_ADC)
	int32_t	result;

	if (!PIOS_INTERNAL_ADC_validate(pios_adc_dev)) {
		return -1;
	}

	/* Check if pin exists */
	if (pin >= pios_adc_dev->cfg->adc_pin_count) {
		return -2;
	}

	if (accumulator[pin].accumulator <= 0) {
		return -3;
	}

	/* return accumulated result and clear accumulator */
	result = accumulator[pin].accumulator / (accumulator[pin].count ?: 1);
	accumulator[pin].accumulator = result;
	accumulator[pin].count = 1;

	return result;
#endif
	return -1;
}

/**
 * @brief Set a callback function that is executed whenever
 * the ADC double buffer swaps 
 * @note Not currently supported.
 */
void PIOS_ADC_SetCallback(ADCCallback new_function)
{
	pios_adc_dev->callback_function = new_function;
}

/**
 * @brief Return the address of the downsampled data buffer
 * @note Not currently supported.
 */
float * PIOS_ADC_GetBuffer(void)
{
	return NULL;
}

/**
 * @brief Return the address of the raw data data buffer 
 * @note Not currently supported.
 */
int16_t * PIOS_ADC_GetRawBuffer(void)
{
	return NULL;
}

/**
 * @brief Return the amount of over sampling
 * @note Not currently supported (always returns 1)
 */
uint8_t PIOS_ADC_GetOverSampling(void)
{
	return 1;
}

/**
 * @brief Set the fir coefficients.  Takes as many samples as the 
 * current filter order plus one (normalization)
 *
 * @param new_filter Array of adc_oversampling floats plus one for the
 * filter coefficients
 * @note Not currently supported.
 */
void PIOS_ADC_SetFIRCoefficients(float * new_filter)
{
	// not implemented
}

/**
 * @brief accumulate the data for each of the channels.
 */
void accumulate(uint16_t *buffer, uint32_t count)
{
#if defined(PIOS_INCLUDE_ADC)
	uint16_t	*sp = buffer;
	if (!PIOS_INTERNAL_ADC_validate(pios_adc_dev)) {
		return;
	}

	/*
	 * Accumulate sampled values.
	 */
	while (count--) {
		for (int i = 0; i < pios_adc_dev->cfg->adc_pin_count; i++) {
			accumulator[i].accumulator += *sp++;
			accumulator[i].count++;
			/*
			 * If the accumulator reaches half-full, rescale in order to
			 * make more space.
			 */
			if (accumulator[i].accumulator >= (1 << 31)) {
				accumulator[i].accumulator /= 2;
				accumulator[i].count /= 2;
			}
		}
	}
	
#if defined(PIOS_INCLUDE_CHIBIOS)
	// XXX should do something with this
	if (pios_adc_dev->data_queue) {
//		bool woken = false;
//		PIOS_Queue_Send_FromISR(adc_dev->data_queue, pios_adc_dev->downsampled_buffer, &woken);
	}

#endif
#endif

//	if(pios_adc_dev->callback_function)
//		pios_adc_dev->callback_function(pios_adc_dev->downsampled_buffer);

}

/**
 * @brief Interrupt on buffer flip.
 *
 * The hardware is done with the 'other' buffer, so we can pass it to the accumulator.
 */
void PIOS_INTERNAL_ADC_DMA_Handler(void)
{
	if (!PIOS_INTERNAL_ADC_validate(pios_adc_dev))
		return;

#if defined(PIOS_INCLUDE_ADC)
	/* terminal count, buffer has flipped */
	if (DMA_GetITStatus(pios_adc_dev->cfg->dma.rx.channel, pios_adc_dev->cfg->full_flag)) {
		DMA_ClearITPendingBit(pios_adc_dev->cfg->dma.rx.channel, pios_adc_dev->cfg->full_flag);

		/* accumulate results from the buffer that was just completed */
		if (DMA_GetCurrentMemoryTarget(pios_adc_dev->cfg->dma.rx.channel) == 0) {
			accumulate(adc_raw_buffer_0, pios_adc_dev->max_samples);
		}
		else {
			accumulate(adc_raw_buffer_1, pios_adc_dev->max_samples);
		}
	}
#endif
}

/**
  * @brief Checks if a given pin is available on the given device
  * \param[in] adc_id handle of the device to read
  * \param[in] device_pin pin to check if available
  * \return true if available
  */
static bool PIOS_INTERNAL_ADC_Available(uint32_t internal_adc_id, uint32_t device_pin) {
	struct pios_internal_adc_dev * adc_dev = (struct pios_internal_adc_dev *)internal_adc_id;
	if(!PIOS_INTERNAL_ADC_validate(adc_dev)) {
			return 0;
	}
	/* Check if pin exists */
	return (!(device_pin >= adc_dev->cfg->adc_pin_count));
}

/**
  * @brief Checks the number of available ADC channels on the device
  * \param[in] adc_id handle of the device
  * \return number of ADC channels of the device
  */
static uint8_t PIOS_INTERNAL_ADC_Number_of_Channels(uint32_t internal_adc_id)
{
	struct pios_internal_adc_dev * adc_dev = (struct pios_internal_adc_dev *)internal_adc_id;
	if(!PIOS_INTERNAL_ADC_validate(adc_dev))
			return 0;
	return adc_dev->cfg->adc_pin_count;
}

/**
 * @brief Gets the least significant bit voltage of the ADC
 */
static float PIOS_INTERNAL_ADC_LSB_Voltage(uint32_t internal_adc_id)
{
	struct pios_internal_adc_dev * adc_dev = (struct pios_internal_adc_dev *) internal_adc_id;
	if (!PIOS_INTERNAL_ADC_validate(adc_dev)) {
		return 0;
	}
	return VREF_PLUS / (((uint32_t)1 << 12) - 1);
}
#endif /* PIOS_INCLUDE_ADC */

/** 
 * @}
 * @}
 */
