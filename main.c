/******************************************************************************
* File Name: main.c
*
* Description: This is the source code for the PSoC 4 MSCLP Multi-touch 
* Mutual-Capacitance
* Touchpad Tuning code example for ModusToolbox.
*
* Related Document: See README.md
*
*******************************************************************************
* $ Copyright 2021-2023 Cypress Semiconductor $
*******************************************************************************/

/*******************************************************************************
 * Include header files
 ******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include "cycfg.h"
#include "cycfg_capsense.h"
#include "user_led_control.h"

/*******************************************************************************
* User Configurable Macro
*******************************************************************************/
/* Enable this, if Serial LED needs to be enabled */
#define ENABLE_SPI_SERIAL_LED           (1u)

/* Enable this, if Tuner needs to be enabled */
#define ENABLE_TUNER                    (1u)

/* Enable widget scan process time measurement */
#define ENABLE_WDGT_PROCESS_TIME_MEASUREMENT (0u)
/*******************************************************************************
* Fixed Macros
*******************************************************************************/
#define CAPSENSE_MSC0_INTR_PRIORITY      (3u)

#define CY_ASSERT_FAILED                 (0u)

/* Define the conditions to check sensor status */
#define SENSOR_ACTIVE                    (1u)

/* EZI2C interrupt priority must be higher than CAPSENSE interrupt. */
#define EZI2C_INTR_PRIORITY              (2u)

/* SysTick macros */
#define TIME_SEC_TO_US                  (1000000u)
#define SYS_TICK_MAX_INTERVAL           (0x00FFFFFF)
#define TIME_PER_TICK_IN_US             ((float)TIME_SEC_TO_US/CY_CAPSENSE_CPU_CLK)

/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Store processing time in micro-seconds */
volatile uint32_t mptx_wgt_process_time;
volatile uint32_t csx_wgt_process_time;

/* EX-I2C system context */
cy_stc_scb_ezi2c_context_t ezi2c_context;

/* LED context structure  */
extern ledDataContext_t userLedContext;

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void initialize_capsense(void);

static void capsense_msc0_isr(void);

static void ezi2c_isr(void);

static void initialize_capsense_tuner(void);

#if ENABLE_SPI_SERIAL_LED
void led_refresh();
#endif

#if ENABLE_WDGT_PROCESS_TIME_MEASUREMENT
/* Process time measurement- function to get SysTick count and return it 
* in micro-sec */
uint32_t get_systick_time(void);
#endif /* ENABLE_WDGT_PROCESS_TIME_MEASUREMENT */

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
*  System entrance point. This function performs
*  - initial setup of device
*  - initialize CAPSENSE
*  - initialize tuner communication
*  - scan touch input continuously
*  - serial RGB LED for touch indication
*
* Return:
*  int
********************************************************************************/
int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Initialize SysTick timer */
    Cy_SysTick_Init (CY_SYSTICK_CLOCK_SOURCE_CLK_CPU ,0x00FFFFFF);

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize SPI master */
    result = init_spi_master();

    /* Initialize EZI2C */
    initialize_capsense_tuner();

    /* Initialize MSCLP CAPSENSE */
    initialize_capsense();

    /* Start the first scan */
    Cy_CapSense_ScanAllSlots(&cy_capsense_context);

    for (;;)
    {
        if(CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
        {

#if ENABLE_WDGT_PROCESS_TIME_MEASUREMENT
            /* Clear SysTick Timer to start afresh measurement*/
            Cy_SysTick_Clear();
            /* Process only MPTX enabled touchpad widget */
            Cy_CapSense_ProcessWidget(CY_CAPSENSE_TOUCHPAD_WDGT_ID,&cy_capsense_context);
            mptx_wgt_process_time = get_systick_time();

            /* Clear SysTick Timer to start afresh measurement*/
            Cy_SysTick_Clear();
            /* Process only regular CSX touchpad widget */
            Cy_CapSense_ProcessWidget(CY_CAPSENSE_TOUCHPAD0_WDGT_ID,&cy_capsense_context);
            csx_wgt_process_time = get_systick_time();
            csx_wgt_process_time = 0;
#else
            /* Process all widgets */
            Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);
#endif

            #if ENABLE_SPI_SERIAL_LED
         /* Serial LED control for showing the CAPSENSE touch status (feedback) */
            led_refresh();
            #endif

            #if ENABLE_TUNER
            /* Establishes synchronized communication with the CAPSENSE Tuner tool */
            Cy_CapSense_RunTuner(&cy_capsense_context);
            #endif

            /* Start the next scan */
            Cy_CapSense_ScanAllSlots(&cy_capsense_context);
        }
    }
}


/*******************************************************************************
* Function Name: initialize_capsense
********************************************************************************
* Summary:
*  This function initializes the CAPSENSE blocks and configures the CAPSENSE
*  interrupt.
*
*******************************************************************************/
static void initialize_capsense(void)
{
    cy_capsense_status_t status = CY_CAPSENSE_STATUS_SUCCESS;

    /* CAPSENSE interrupt configuration MSCLP 0 */
    const cy_stc_sysint_t capsense_msc0_interrupt_config =
    {
        .intrSrc = CY_MSCLP0_LP_IRQ,
        .intrPriority = CAPSENSE_MSC0_INTR_PRIORITY,
    };

    /* Capture the MSC HW block and initialize it to the default state. */
    status = Cy_CapSense_Init(&cy_capsense_context);

    if (CY_CAPSENSE_STATUS_SUCCESS == status)
    {
        /* Initialize CAPSENSE interrupt for MSCLP 0 */
        Cy_SysInt_Init(&capsense_msc0_interrupt_config, capsense_msc0_isr);
        NVIC_ClearPendingIRQ(capsense_msc0_interrupt_config.intrSrc);
        NVIC_EnableIRQ(capsense_msc0_interrupt_config.intrSrc);
        

        /* Initialize the CAPSENSE firmware modules. */
        status = Cy_CapSense_Enable(&cy_capsense_context);
    }

    if(status != CY_CAPSENSE_STATUS_SUCCESS)
    {
        /* This status could fail before tuning the sensors correctly.
         * Ensure that this function passes after the CAPSENSE sensors are tuned
         * as per procedure give in the Readme.md file */
    }
}

/*******************************************************************************
* Function Name: capsense_msc0_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CAPSENSE MSC0 block.
*
*******************************************************************************/
static void capsense_msc0_isr(void)
{
    Cy_CapSense_InterruptHandler(CY_MSCLP0_HW, &cy_capsense_context);
}


/*******************************************************************************
* Function Name: initialize_capsense_tuner
********************************************************************************
* Summary:
* EZI2C module to communicate with the CAPSENSE Tuner tool.
*
*******************************************************************************/
static void initialize_capsense_tuner(void)
{
    cy_en_scb_ezi2c_status_t status = CY_SCB_EZI2C_SUCCESS;

    /* EZI2C interrupt configuration structure */
    const cy_stc_sysint_t ezi2c_intr_config =
    {
        .intrSrc = CYBSP_EZI2C_IRQ,
        .intrPriority = EZI2C_INTR_PRIORITY,
    };

    /* Initialize the EZI2C firmware module */
    status = Cy_SCB_EZI2C_Init(CYBSP_EZI2C_HW, &CYBSP_EZI2C_config, &ezi2c_context);

    if(status != CY_SCB_EZI2C_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    Cy_SysInt_Init(&ezi2c_intr_config, ezi2c_isr);
    NVIC_EnableIRQ(ezi2c_intr_config.intrSrc);

    /* Set the CAPSENSE data structure as the I2C buffer to be exposed to the
     * master on primary slave address interface. Any I2C host tools such as
     * the Tuner or the Bridge Control Panel can read this buffer but you can
     * connect only one tool at a time. */
    Cy_SCB_EZI2C_SetBuffer1(CYBSP_EZI2C_HW, (uint8_t *)&cy_capsense_tuner,
                            sizeof(cy_capsense_tuner), sizeof(cy_capsense_tuner),
                            &ezi2c_context);

    Cy_SCB_EZI2C_Enable(CYBSP_EZI2C_HW);
}

/*******************************************************************************
* Function Name: ezi2c_isr
********************************************************************************
* Summary:
* Wrapper function for handling interrupts from EZI2C block.
*
*******************************************************************************/
static void ezi2c_isr(void)
{
    Cy_SCB_EZI2C_Interrupt(CYBSP_EZI2C_HW, &ezi2c_context);
}


#if ENABLE_SPI_SERIAL_LED
/*******************************************************************************
* Function Name: led_refresh
********************************************************************************
* Summary:
* Logic to control the on / off status with green color and brightness of LED1 
* and LED3 based on the touch status of the CAPSENSE touchpad widget.
* Brightness of each LED is represented by 0 to 255, where 0 indicates LED in 
* OFF state and 255 indicate maximum brightness of an LED
*******************************************************************************/
void led_refresh()
{
   cy_stc_capsense_touch_t *panelTouch=NULL;

    /* If the CSD Touchpad is active, Turn On LED1 and LED3 with Green color, and 
    * vary the intensity of the LED as per the finger position reported */

    userLedContext.userLedData[LED1].green = 0;
    userLedContext.userLedData[LED3].green = 0;

    if (SENSOR_ACTIVE == Cy_CapSense_IsWidgetActive(CY_CAPSENSE_TOUCHPAD_WDGT_ID, &cy_capsense_context))
    {
        panelTouch = Cy_CapSense_GetTouchInfo(CY_CAPSENSE_TOUCHPAD_WDGT_ID, &   cy_capsense_context);

        userLedContext.userLedData[LED1].green = panelTouch->ptrPosition->x;
        userLedContext.userLedData[LED3].green = panelTouch->ptrPosition->y;
    }
    process_spi_led_data(&userLedContext);
}
#endif

#if ENABLE_WDGT_PROCESS_TIME_MEASUREMENT
/*******************************************************************************
* Function Name: get_systick_time
********************************************************************************
* Summary:
*  Reads the system tick and converts to time in microseconds(us).
*
*  Returns:
*  runtime - in microseconds(us)
*******************************************************************************/
uint32_t get_systick_time(void)
{
    uint32_t ticks;
    uint32_t runtime;
    ticks = Cy_SysTick_GetValue();
    ticks = SYS_TICK_MAX_INTERVAL - Cy_SysTick_GetValue();
    runtime = (uint32_t)((float)ticks * TIME_PER_TICK_IN_US);
    return runtime;
}
#endif

/* [] END OF FILE */
