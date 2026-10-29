/* Project/API reading notes: Standalone non-destructive CAN test protocol.
 * Discovery, echo, patterns, statistics and replay handling verify communication without
 * erasing/programming App Flash.
 * FD payload length and CRC checks remain part of the request contract.
 */
#include <stdio.h>
#include <string.h>

#include "inc_main.h"
#include "can_transfer_config.h"
#include "can_transfer_protocol.h"

#define CAN_TRANSFER_TX_IDLE (0U)
#define CAN_TRANSFER_TX_SENDING (1U)
#define CAN_TRANSFER_TX_ABORTING (2U)
#define CAN_TRANSFER_TX_FAULT (3U)

#define CAN_TRANSFER_TX_TIMEOUT_MS (100U)
#define CAN_TRANSFER_ABORT_TIMEOUT_MS (100U)
#define CAN_TRANSFER_ERROR_TRACE_INTERVAL_MS (250U)
#define CAN_TRANSFER_TICK_WRAP_MS (60000U)

#define CAN_TRANSFER_INFO_PAGE_VERSION (0U)
#define CAN_TRANSFER_INFO_PAGE_TARGET (1U)
#define CAN_TRANSFER_INFO_PAGE_PART_0 (2U)
#define CAN_TRANSFER_INFO_PAGE_PART_1 (3U)
#define CAN_TRANSFER_INFO_PAGE_PART_2 (4U)
#define CAN_TRANSFER_INFO_PAGE_PART_3 (5U)
#define CAN_TRANSFER_INFO_PAGE_CAPABILITIES (6U)

#define CAN_TRANSFER_STATS_RX_FRAMES (0U)
#define CAN_TRANSFER_STATS_TX_FRAMES (1U)
#define CAN_TRANSFER_STATS_RX_LOST (2U)
#define CAN_TRANSFER_STATS_TX_FAILURES (3U)
#define CAN_TRANSFER_STATS_PROTOCOL_ERRORS (4U)
#define CAN_TRANSFER_STATS_CRC_ERRORS (5U)
#define CAN_TRANSFER_STATS_DUPLICATES (6U)
#define CAN_TRANSFER_STATS_WARNING_ENTRIES (7U)
#define CAN_TRANSFER_STATS_PASSIVE_ENTRIES (8U)
#define CAN_TRANSFER_STATS_BUS_OFF_ENTRIES (9U)
#define CAN_TRANSFER_STATS_LAST_DRIVER_RESULT (10U)
#define CAN_TRANSFER_STATS_LAST_ERROR_FLAGS (11U)

#define CAN_TRANSFER_ERROR_BUS_FLAG (0x0001U)
#define CAN_TRANSFER_ERROR_WARNING_FLAG (0x0002U)
#define CAN_TRANSFER_ERROR_PASSIVE_FLAG (0x0004U)
#define CAN_TRANSFER_ERROR_BUS_OFF_FLAG (0x0008U)
#define CAN_TRANSFER_ERROR_RECOVERY_FLAG (0x0010U)
#define CAN_TRANSFER_ERROR_OVERLOAD_FLAG (0x0020U)
#define CAN_TRANSFER_ERROR_BUS_LOCK_FLAG (0x0040U)
#define CAN_TRANSFER_ERROR_ARBITRATION_FLAG (0x0080U)
#define CAN_TRANSFER_ERROR_STUFF_FLAG (0x0100U)
#define CAN_TRANSFER_ERROR_FORM_FLAG (0x0200U)
#define CAN_TRANSFER_ERROR_ACK_FLAG (0x0400U)
#define CAN_TRANSFER_ERROR_CRC_FLAG (0x0800U)
#define CAN_TRANSFER_ERROR_RECESSIVE_FLAG (0x1000U)
#define CAN_TRANSFER_ERROR_DOMINANT_FLAG (0x2000U)
#define CAN_TRANSFER_ERROR_ACK_DELIMITER_FLAG (0x4000U)

#define CAN_TRANSFER_CLEAR_KEY_0 (0xC3U)
#define CAN_TRANSFER_CLEAR_KEY_1 (0x3CU)

typedef struct
{
    volatile uint16_t rx_frames;
    volatile uint16_t tx_frames;
    volatile uint16_t rx_lost;
    volatile uint16_t tx_failures;
    volatile uint16_t protocol_errors;
    volatile uint16_t crc_errors;
    volatile uint16_t duplicates;
    volatile uint16_t warning_entries;
    volatile uint16_t passive_entries;
    volatile uint16_t bus_off_entries;
    volatile uint16_t last_driver_result;
    volatile uint16_t last_error_flags;
} can_transfer_stats_t;

static can_transfer_stats_t g_stats;
static uint8_t g_hardware_ready;
static uint8_t g_tx_state;
static uint16_t g_tx_started_at;
static uint8_t g_cache_valid;
static uint8_t g_cached_request[CAN_TRANSFER_FRAME_SIZE];
static uint8_t g_cached_response[CAN_TRANSFER_FRAME_SIZE];
static volatile uint16_t g_pending_error_flags;
static uint8_t g_error_trace_started;
static uint16_t g_error_trace_at;

extern unsigned int get_tick(void);

static void increment_u16(volatile uint16_t *value)
{
    if (*value != 0xFFFFU)
    {
        (*value)++;
    }
}

static uint16_t elapsed_ms(uint16_t started_at, uint16_t now)
{
    if (now >= started_at)
    {
        return (uint16_t)(now - started_at);
    }

    return (uint16_t)(CAN_TRANSFER_TICK_WRAP_MS - started_at + now);
}

static uint8_t crc8_atm(const uint8_t *data, uint8_t length)
{
    uint8_t crc;
    uint8_t byte_index;
    uint8_t bit_index;

    crc = 0U;
    for (byte_index = 0U; byte_index < length; byte_index++)
    {
        crc ^= data[byte_index];
        for (bit_index = 0U; bit_index < 8U; bit_index++)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static void trace_frame(const char *direction, const can_frame_t *frame)
{
#if (CAN_TRANSFER_UART_TRACE_ENABLE != 0U)
    uint8_t index;
    uint8_t data_length;
    const char *crc_result;

    data_length =
        (frame->DLC <= CAN_TRANSFER_FRAME_SIZE) ? (uint8_t)frame->DLC : CAN_TRANSFER_FRAME_SIZE;
    crc_result = "N/A";
    if (data_length == CAN_TRANSFER_FRAME_SIZE)
    {
        crc_result = (crc8_atm(frame->DB, 7U) == frame->DB[7]) ? "OK" : "BAD";
    }

    printf("TRACE CT %s id=0x%03X dlc=%u data=[", direction, (unsigned int)frame->IDL,
           (unsigned int)frame->DLC);
    for (index = 0U; index < data_length; index++)
    {
        if (index != 0U)
        {
            putchar(' ');
        }
        printf("%02X", (unsigned int)frame->DB[index]);
    }
    printf("] seq=%u command=0x%02X crc=%s\r\n",
           (data_length > 2U) ? (unsigned int)frame->DB[2] : 0U,
           (data_length > 3U) ? (unsigned int)frame->DB[3] : 0U, crc_result);
#else
    (void)direction;
    (void)frame;
#endif
}

static void trace_pending_channel_error(void)
{
#if (CAN_TRANSFER_UART_TRACE_ENABLE != 0U)
    uint16_t error_flags;
    uint16_t now;

    DI();
    error_flags = g_pending_error_flags;
    EI();

    if (error_flags == 0U)
    {
        return;
    }

    now = (uint16_t)get_tick();
    if ((g_error_trace_started != 0U) &&
        (elapsed_ms(g_error_trace_at, now) < CAN_TRANSFER_ERROR_TRACE_INTERVAL_MS))
    {
        return;
    }

    DI();
    error_flags = g_pending_error_flags;
    g_pending_error_flags = 0U;
    EI();
    if (error_flags == 0U)
    {
        return;
    }
    g_error_trace_started = 1U;
    g_error_trace_at = now;

    printf("TRACE CT CAN_ERROR flags=0x%04X decoded=[", (unsigned int)error_flags);
    if ((error_flags & CAN_TRANSFER_ERROR_BUS_FLAG) != 0U)
    {
        printf(" BUS_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_WARNING_FLAG) != 0U)
    {
        printf(" ERROR_WARNING");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_PASSIVE_FLAG) != 0U)
    {
        printf(" ERROR_PASSIVE");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_BUS_OFF_FLAG) != 0U)
    {
        printf(" BUS_OFF_ENTRY");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_RECOVERY_FLAG) != 0U)
    {
        printf(" BUS_OFF_RECOVERY");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_OVERLOAD_FLAG) != 0U)
    {
        printf(" OVERLOAD");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_BUS_LOCK_FLAG) != 0U)
    {
        printf(" BUS_LOCK");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_ARBITRATION_FLAG) != 0U)
    {
        printf(" ARBITRATION_LOST");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_STUFF_FLAG) != 0U)
    {
        printf(" STUFF_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_FORM_FLAG) != 0U)
    {
        printf(" FORM_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_ACK_FLAG) != 0U)
    {
        printf(" ACK_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_CRC_FLAG) != 0U)
    {
        printf(" CRC_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_RECESSIVE_FLAG) != 0U)
    {
        printf(" RECESSIVE_BIT_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_DOMINANT_FLAG) != 0U)
    {
        printf(" DOMINANT_BIT_ERROR");
    }
    if ((error_flags & CAN_TRANSFER_ERROR_ACK_DELIMITER_FLAG) != 0U)
    {
        printf(" ACK_DELIMITER_ERROR");
    }
    printf(" ]\r\n");
#endif
}

static void clear_stats(void)
{
    DI();
    g_stats.rx_frames = 0U;
    g_stats.tx_frames = 0U;
    g_stats.rx_lost = 0U;
    g_stats.tx_failures = 0U;
    g_stats.protocol_errors = 0U;
    g_stats.crc_errors = 0U;
    g_stats.duplicates = 0U;
    g_stats.warning_entries = 0U;
    g_stats.passive_entries = 0U;
    g_stats.bus_off_entries = 0U;
    g_stats.last_driver_result = CAN_RTN_OK;
    g_stats.last_error_flags = 0U;
    g_pending_error_flags = 0U;
    g_error_trace_started = 0U;
    g_error_trace_at = 0U;
    EI();
}

static uint16_t get_stat_value(uint8_t index)
{
    uint16_t value;

    value = 0U;
    DI();
    switch (index)
    {
    case CAN_TRANSFER_STATS_RX_FRAMES:
        value = g_stats.rx_frames;
        break;
    case CAN_TRANSFER_STATS_TX_FRAMES:
        value = g_stats.tx_frames;
        break;
    case CAN_TRANSFER_STATS_RX_LOST:
        value = g_stats.rx_lost;
        break;
    case CAN_TRANSFER_STATS_TX_FAILURES:
        value = g_stats.tx_failures;
        break;
    case CAN_TRANSFER_STATS_PROTOCOL_ERRORS:
        value = g_stats.protocol_errors;
        break;
    case CAN_TRANSFER_STATS_CRC_ERRORS:
        value = g_stats.crc_errors;
        break;
    case CAN_TRANSFER_STATS_DUPLICATES:
        value = g_stats.duplicates;
        break;
    case CAN_TRANSFER_STATS_WARNING_ENTRIES:
        value = g_stats.warning_entries;
        break;
    case CAN_TRANSFER_STATS_PASSIVE_ENTRIES:
        value = g_stats.passive_entries;
        break;
    case CAN_TRANSFER_STATS_BUS_OFF_ENTRIES:
        value = g_stats.bus_off_entries;
        break;
    case CAN_TRANSFER_STATS_LAST_DRIVER_RESULT:
        value = g_stats.last_driver_result;
        break;
    case CAN_TRANSFER_STATS_LAST_ERROR_FLAGS:
        value = g_stats.last_error_flags;
        break;
    default:
        break;
    }
    EI();

    return value;
}

static void build_response(uint8_t sequence, uint8_t command, uint8_t payload0, uint8_t payload1,
                           uint8_t payload2, uint8_t *response)
{
    response[0] = CAN_TRANSFER_RESPONSE_MAGIC;
    response[1] = CAN_TRANSFER_PROTOCOL_MAJOR;
    response[2] = sequence;
    response[3] = command;
    response[4] = payload0;
    response[5] = payload1;
    response[6] = payload2;
    response[7] = crc8_atm(response, 7U);
}

static void build_error_response(uint8_t sequence, uint8_t request_command, uint8_t status,
                                 uint8_t detail, uint8_t *response)
{
    build_response(sequence, CAN_TRANSFER_CMD_ERROR_RESPONSE, request_command, status, detail,
                   response);
}

static uint8_t start_transmit(const uint8_t *response)
{
    can_frame_t frame;
    Can_RtnType result;
    uint8_t index;

    if (g_tx_state != CAN_TRANSFER_TX_IDLE)
    {
        return 0U;
    }

    memset(&frame, 0, sizeof(frame));
    frame.IDE = 0U;
    frame.RTR = 0U;
    frame.THDSE = 0U;
    frame.IDL = CAN_TRANSFER_RESPONSE_ID;
    frame.IDH = 0U;
    frame.DLC = CAN_TRANSFER_FRAME_SIZE;
    for (index = 0U; index < CAN_TRANSFER_FRAME_SIZE; index++)
    {
        frame.DB[index] = response[index];
    }

    result = R_CAN_TrmByTxBuf(CAN_CH0, CAN_TXBUF0, &frame);
    g_stats.last_driver_result = result;
    if (result != CAN_RTN_OK)
    {
        increment_u16(&g_stats.tx_failures);
        return 0U;
    }

    trace_frame("TX", &frame);
    increment_u16(&g_stats.tx_frames);
    g_tx_started_at = (uint16_t)get_tick();
    g_tx_state = CAN_TRANSFER_TX_SENDING;
    return 1U;
}

static void cache_and_transmit(const can_frame_t *request, const uint8_t *response)
{
    memcpy(g_cached_request, request->DB, CAN_TRANSFER_FRAME_SIZE);
    memcpy(g_cached_response, response, CAN_TRANSFER_FRAME_SIZE);
    g_cache_valid = 1U;
    (void)start_transmit(response);
}

static void poll_transmit(void)
{
    Can_RtnType result;
    uint16_t now;

    if (g_tx_state == CAN_TRANSFER_TX_IDLE)
    {
        return;
    }

    result = R_CAN_CheckTxBufResult(CAN_CH0, CAN_TXBUF0);
    g_stats.last_driver_result = result;
    if (result == CAN_RTN_TX_END)
    {
        g_tx_state = CAN_TRANSFER_TX_IDLE;
        return;
    }

    if ((result == CAN_RTN_TX_ABORT_OVER) || (result == CAN_RTN_TX_END_WITH_ABORT_REQ))
    {
        increment_u16(&g_stats.tx_failures);
        g_tx_state = CAN_TRANSFER_TX_IDLE;
        return;
    }

    if (result != CAN_RTN_TRANSMITTING)
    {
        increment_u16(&g_stats.tx_failures);
        g_tx_state = CAN_TRANSFER_TX_IDLE;
        return;
    }

    now = (uint16_t)get_tick();
    if ((g_tx_state == CAN_TRANSFER_TX_SENDING) &&
        (elapsed_ms(g_tx_started_at, now) >= CAN_TRANSFER_TX_TIMEOUT_MS))
    {
        result = R_CAN_AbortTrm(CAN_CH0, CAN_TXBUF0);
        g_stats.last_driver_result = result;
        if (result == CAN_RTN_OK)
        {
            g_tx_started_at = now;
            g_tx_state = CAN_TRANSFER_TX_ABORTING;
        }
        else
        {
            increment_u16(&g_stats.tx_failures);
            g_tx_state = CAN_TRANSFER_TX_FAULT;
        }
    }
    else if ((g_tx_state == CAN_TRANSFER_TX_ABORTING) &&
             (elapsed_ms(g_tx_started_at, now) >= CAN_TRANSFER_ABORT_TIMEOUT_MS))
    {
        increment_u16(&g_stats.tx_failures);
        g_tx_state = CAN_TRANSFER_TX_FAULT;
    }
}

static uint8_t build_device_info(const can_frame_t *request, uint8_t *response)
{
    uint8_t page;
    uint8_t value0;
    uint8_t value1;

    page = request->DB[4];
    value0 = 0U;
    value1 = 0U;
    if ((request->DB[5] != 0U) || (request->DB[6] != 0U))
    {
        return 0U;
    }

    switch (page)
    {
    case CAN_TRANSFER_INFO_PAGE_VERSION:
        value0 = CAN_TRANSFER_PROTOCOL_MAJOR;
        value1 = CAN_TRANSFER_PROTOCOL_MINOR;
        break;
    case CAN_TRANSFER_INFO_PAGE_TARGET:
        value0 = (uint8_t)(CAN_TRANSFER_TARGET_ID & 0xFFU);
        value1 = (uint8_t)(CAN_TRANSFER_TARGET_ID >> 8);
        break;
    case CAN_TRANSFER_INFO_PAGE_PART_0:
        value0 = (uint8_t)'R';
        value1 = (uint8_t)'5';
        break;
    case CAN_TRANSFER_INFO_PAGE_PART_1:
        value0 = (uint8_t)'F';
        value1 = (uint8_t)'1';
        break;
    case CAN_TRANSFER_INFO_PAGE_PART_2:
        value0 = (uint8_t)'0';
        value1 = (uint8_t)'B';
        break;
    case CAN_TRANSFER_INFO_PAGE_PART_3:
        value0 = (uint8_t)'M';
        value1 = (uint8_t)'G';
        break;
    case CAN_TRANSFER_INFO_PAGE_CAPABILITIES:
        value0 = 0x3FU;
        value1 = 0U;
        break;
    default:
        return 0U;
    }

    build_response(request->DB[2], request->DB[3], page, value0, value1, response);
    return 1U;
}

static uint8_t dispatch_request(const can_frame_t *request, uint8_t *response)
{
    uint8_t command;
    uint8_t index;
    uint16_t value;

    command = request->DB[3];
    switch (command)
    {
    case CAN_TRANSFER_CMD_DEVICE_INFO:
        return build_device_info(request, response);

    case CAN_TRANSFER_CMD_PING:
    case CAN_TRANSFER_CMD_ECHO:
        build_response(request->DB[2], command, request->DB[4], request->DB[5], request->DB[6],
                       response);
        return 1U;

    case CAN_TRANSFER_CMD_PATTERN_TEST:
        build_response(request->DB[2], command, (uint8_t)(request->DB[4] ^ 0xA5U),
                       (uint8_t)(request->DB[5] + 0x3DU),
                       (uint8_t)((request->DB[6] << 1) | (request->DB[6] >> 7)), response);
        return 1U;

    case CAN_TRANSFER_CMD_GET_STATS:
        index = request->DB[4];
        if ((index > CAN_TRANSFER_STATS_LAST_ERROR_FLAGS) || (request->DB[5] != 0U) ||
            (request->DB[6] != 0U))
        {
            return 0U;
        }
        value = get_stat_value(index);
        build_response(request->DB[2], command, index, (uint8_t)(value & 0xFFU),
                       (uint8_t)(value >> 8), response);
        return 1U;

    case CAN_TRANSFER_CMD_CLEAR_STATS:
        if ((request->DB[4] != CAN_TRANSFER_CLEAR_KEY_0) ||
            (request->DB[5] != CAN_TRANSFER_CLEAR_KEY_1) || (request->DB[6] != 0U))
        {
            return 0U;
        }
        clear_stats();
        build_response(request->DB[2], command, CAN_TRANSFER_CLEAR_KEY_0, CAN_TRANSFER_CLEAR_KEY_1,
                       0U, response);
        return 1U;

    default:
        build_error_response(request->DB[2], command, CAN_TRANSFER_STATUS_BAD_COMMAND, 0U,
                             response);
        return 2U;
    }
}

static void process_receive(void)
{
    can_frame_t request;
    Can_RtnType result;
    uint8_t response[CAN_TRANSFER_FRAME_SIZE];
    uint8_t dispatch_result;

    if (g_tx_state != CAN_TRANSFER_TX_IDLE)
    {
        return;
    }

    result = R_CAN_ReadRxFIFO(CAN_RXFIFO0, &request);
    g_stats.last_driver_result = result;
    if (result == CAN_RTN_BUFFER_EMPTY)
    {
        return;
    }
    if ((result != CAN_RTN_OK) && (result != CAN_RTN_OK_WITH_LOST))
    {
        increment_u16(&g_stats.protocol_errors);
        return;
    }

    trace_frame("RX", &request);

    increment_u16(&g_stats.rx_frames);
    if (result == CAN_RTN_OK_WITH_LOST)
    {
        increment_u16(&g_stats.rx_lost);
    }

    if ((request.IDE != 0U) || (request.RTR != 0U) || (request.IDL != CAN_TRANSFER_REQUEST_ID) ||
        (request.IDH != 0U))
    {
        increment_u16(&g_stats.protocol_errors);
        return;
    }

    if (request.DLC != CAN_TRANSFER_FRAME_SIZE)
    {
        increment_u16(&g_stats.protocol_errors);
        return;
    }

    if (crc8_atm(request.DB, 7U) != request.DB[7])
    {
        increment_u16(&g_stats.crc_errors);
        increment_u16(&g_stats.protocol_errors);
        build_error_response(request.DB[2], request.DB[3], CAN_TRANSFER_STATUS_CRC_ERROR, 0U,
                             response);
        cache_and_transmit(&request, response);
        return;
    }

    if (request.DB[0] != CAN_TRANSFER_REQUEST_MAGIC)
    {
        increment_u16(&g_stats.protocol_errors);
        build_error_response(request.DB[2], request.DB[3], CAN_TRANSFER_STATUS_BAD_MAGIC,
                             request.DB[0], response);
        cache_and_transmit(&request, response);
        return;
    }

    if (request.DB[1] != CAN_TRANSFER_PROTOCOL_MAJOR)
    {
        increment_u16(&g_stats.protocol_errors);
        build_error_response(request.DB[2], request.DB[3], CAN_TRANSFER_STATUS_BAD_VERSION,
                             request.DB[1], response);
        cache_and_transmit(&request, response);
        return;
    }

    if (request.DB[2] == 0U)
    {
        increment_u16(&g_stats.protocol_errors);
        build_error_response(request.DB[2], request.DB[3], CAN_TRANSFER_STATUS_BAD_SEQUENCE, 0U,
                             response);
        cache_and_transmit(&request, response);
        return;
    }

    if ((g_cache_valid != 0U) &&
        (memcmp(g_cached_request, request.DB, CAN_TRANSFER_FRAME_SIZE) == 0))
    {
        increment_u16(&g_stats.duplicates);
        (void)start_transmit(g_cached_response);
        return;
    }

    if ((g_cache_valid != 0U) && (request.DB[2] == g_cached_request[2]))
    {
        increment_u16(&g_stats.protocol_errors);
        build_error_response(request.DB[2], request.DB[3], CAN_TRANSFER_STATUS_BAD_SEQUENCE, 0U,
                             response);
        cache_and_transmit(&request, response);
        return;
    }

    dispatch_result = dispatch_request(&request, response);
    if (dispatch_result == 0U)
    {
        increment_u16(&g_stats.protocol_errors);
        build_error_response(request.DB[2], request.DB[3], CAN_TRANSFER_STATUS_BAD_ARGUMENT, 0U,
                             response);
    }
    else if (dispatch_result == 2U)
    {
        increment_u16(&g_stats.protocol_errors);
    }
    cache_and_transmit(&request, response);
}

void CAN_TransferProtocol_Init(Can_RtnType hardware_result)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_cached_request, 0, sizeof(g_cached_request));
    memset(g_cached_response, 0, sizeof(g_cached_response));
    g_hardware_ready = (hardware_result == CAN_RTN_OK) ? 1U : 0U;
    g_tx_state = CAN_TRANSFER_TX_IDLE;
    g_tx_started_at = 0U;
    g_cache_valid = 0U;
    g_pending_error_flags = 0U;
    g_error_trace_started = 0U;
    g_error_trace_at = 0U;
    g_stats.last_driver_result = hardware_result;
}

void CAN_TransferProtocol_Process(void)
{
    trace_pending_channel_error();

    if (g_hardware_ready == 0U)
    {
        return;
    }

    poll_transmit();
    process_receive();
}

void CAN_TransferProtocol_OnChannelError(uint16_t error_flags)
{
    g_stats.last_error_flags = error_flags;
    g_pending_error_flags |= error_flags;
    if ((error_flags & CAN_TRANSFER_ERROR_WARNING_FLAG) != 0U)
    {
        increment_u16(&g_stats.warning_entries);
    }
    if ((error_flags & CAN_TRANSFER_ERROR_PASSIVE_FLAG) != 0U)
    {
        increment_u16(&g_stats.passive_entries);
    }
    if ((error_flags & CAN_TRANSFER_ERROR_BUS_OFF_FLAG) != 0U)
    {
        increment_u16(&g_stats.bus_off_entries);
    }
}
