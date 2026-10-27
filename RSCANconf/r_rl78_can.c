/***********************************************************************************************************************
Pragma directive
***********************************************************************************************************************/
/* Start user code for pragma. Do not edit comment generated here */

/* End user code. Do not edit comment generated here */

/***********************************************************************************************************************
Includes
***********************************************************************************************************************/
#include "r_cg_macrodriver.h"
#include "r_cg_port.h"
/* Start user code for include. Do not edit comment generated here */
#include "r_rl78_can_drv.h" /* Header file for CAN software driver */
/* End user code. Do not edit comment generated here */
#include "r_cg_userdefine.h"

/***********************************************************************************************************************
Global variables and functions
***********************************************************************************************************************/
/* Start user code for global. Do not edit comment generated here */
/* End user code. Do not edit comment generated here */

/* Start user code for adding. Do not edit comment generated here */
/* End user code. Do not edit comment generated here */

#define CAN_CREATE_POLL_LIMIT (60000UL)

Can_RtnType R_CAN_Create(void)
{
    Can_RtnType retval;
    uint32_t poll_count;

    /* supply CAN clock */
    CAN0EN = 1;

    /* CAN Initialize */
    retval = CAN_RTN_RAM_INIT;
    poll_count = 0UL;
    while ((retval != CAN_RTN_OK) && (poll_count < CAN_CREATE_POLL_LIMIT))
    {
        retval = R_CAN_Init();
        poll_count++;
    }
    if (retval != CAN_RTN_OK)
    {
        return retval;
    }

    /* Set global operating mode */
    retval = CAN_RTN_MODE_WAIT;
    poll_count = 0UL;
    while ((retval == CAN_RTN_MODE_WAIT) && (poll_count < CAN_CREATE_POLL_LIMIT))
    {
        retval = R_CAN_GlobalStart();
        poll_count++;
    }
    if (retval != CAN_RTN_OK)
    {
        return retval;
    }

    /* CH0 -> Channel communication mode */
    retval = CAN_RTN_MODE_WAIT;
    poll_count = 0UL;
    while ((retval == CAN_RTN_MODE_WAIT) && (poll_count < CAN_CREATE_POLL_LIMIT))
    {
        retval = R_CAN_ChStart(CAN_CH0);
        poll_count++;
    }
    if (retval != CAN_RTN_OK)
    {
        return retval;
    }

    /* Set interrupt */
    CAN0TRMMK = 1; /* TX completion is polled by the main-loop state machine. */
    CAN0CFRMK = 1; /* The unused common RX FIFO interrupt remains masked. */
    CAN0ERRMK = 0; /* Enable channel error interrupt mask flag */
    CANGRFRMK = 1; /* RX FIFO is drained by the main loop. */
    CANGERRMK = 1; /* No global error source is enabled. */

    return CAN_RTN_OK;
}
