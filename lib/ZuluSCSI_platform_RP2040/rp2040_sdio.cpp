// Implementation of SDIO communication for RP2040
//
// The RP2040 official work-in-progress code at
// https://github.com/raspberrypi/pico-extras/tree/master/src/rp2_common/pico_sd_card
// may be useful reference, but this is independent implementation.
//
// For official SDIO specifications, refer to:
// https://www.sdcard.org/downloads/pls/
// "SDIO Physical Layer Simplified Specification Version 8.00"

#include "rp2040_sdio.h"
#include "rp2040_sdio.pio.h"
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <ZuluSCSI_platform.h>
#include <ZuluSCSI_log.h>

#define SDIO_PIO pio1
#define SDIO_CMD_SM 0
#define SDIO_DATA_SM 1
#define SDIO_DMA_CH 1

// Maximum number of 512 byte blocks to transfer in one request
#define SDIO_MAX_BLOCKS 256

enum sdio_transfer_state_t { SDIO_IDLE, SDIO_RX, SDIO_TX };

static struct {
    uint32_t pio_cmd_clk_offset;
    uint32_t pio_data_rx_offset;
    pio_sm_config pio_cfg_data_rx;
    uint32_t pio_data_tx_offset;
    pio_sm_config pio_cfg_data_tx;

    sdio_transfer_state_t transfer_state;
    bool inside_irq_handler; // True if we are inside crash handler code
    uint32_t transfer_start_time;
    uint32_t *data_buf;
    uint32_t blocks_done; // Number of blocks transferred so far
    uint32_t total_blocks; // Total number of blocks to transfer
    uint32_t blocks_checksumed; // Number of blocks that have had CRC calculated
    uint32_t checksum_errors; // Number of checksum errors detected
    uint64_t block_checksums[SDIO_MAX_BLOCKS];
} g_sdio;

/*******************************************************
 * Checksum algorithms
 *******************************************************/

// Table lookup for calculating CRC-7 checksum that is used in SDIO command packets.
// Usage:
//    uint8_t crc = 0;
//    crc = crc7_table[crc ^ byte];
//    .. repeat for every byte ..
static const uint8_t crc7_table[256] = {
	0x00, 0x12, 0x24, 0x36, 0x48, 0x5a, 0x6c, 0x7e,	0x90, 0x82, 0xb4, 0xa6, 0xd8, 0xca, 0xfc, 0xee,
	0x32, 0x20, 0x16, 0x04, 0x7a, 0x68, 0x5e, 0x4c,	0xa2, 0xb0, 0x86, 0x94, 0xea, 0xf8, 0xce, 0xdc,
	0x64, 0x76, 0x40, 0x52, 0x2c, 0x3e, 0x08, 0x1a,	0xf4, 0xe6, 0xd0, 0xc2, 0xbc, 0xae, 0x98, 0x8a,
	0x56, 0x44, 0x72, 0x60, 0x1e, 0x0c, 0x3a, 0x28,	0xc6, 0xd4, 0xe2, 0xf0, 0x8e, 0x9c, 0xaa, 0xb8,
	0xc8, 0xda, 0xec, 0xfe, 0x80, 0x92, 0xa4, 0xb6,	0x58, 0x4a, 0x7c, 0x6e, 0x10, 0x02, 0x34, 0x26,
	0xfa, 0xe8, 0xde, 0xcc, 0xb2, 0xa0, 0x96, 0x84,	0x6a, 0x78, 0x4e, 0x5c, 0x22, 0x30, 0x06, 0x14,
	0xac, 0xbe, 0x88, 0x9a, 0xe4, 0xf6, 0xc0, 0xd2,	0x3c, 0x2e, 0x18, 0x0a, 0x74, 0x66, 0x50, 0x42,
	0x9e, 0x8c, 0xba, 0xa8, 0xd6, 0xc4, 0xf2, 0xe0,	0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x70,
	0x82, 0x90, 0xa6, 0xb4, 0xca, 0xd8, 0xee, 0xfc,	0x12, 0x00, 0x36, 0x24, 0x5a, 0x48, 0x7e, 0x6c,
	0xb0, 0xa2, 0x94, 0x86, 0xf8, 0xea, 0xdc, 0xce,	0x20, 0x32, 0x04, 0x16, 0x68, 0x7a, 0x4c, 0x5e,
	0xe6, 0xf4, 0xc2, 0xd0, 0xae, 0xbc, 0x8a, 0x98,	0x76, 0x64, 0x52, 0x40, 0x3e, 0x2c, 0x1a, 0x08,
	0xd4, 0xc6, 0xf0, 0xe2, 0x9c, 0x8e, 0xb8, 0xaa,	0x44, 0x56, 0x60, 0x72, 0x0c, 0x1e, 0x28, 0x3a,
	0x4a, 0x58, 0x6e, 0x7c, 0x02, 0x10, 0x26, 0x34,	0xda, 0xc8, 0xfe, 0xec, 0x92, 0x80, 0xb6, 0xa4,
	0x78, 0x6a, 0x5c, 0x4e, 0x30, 0x22, 0x14, 0x06,	0xe8, 0xfa, 0xcc, 0xde, 0xa0, 0xb2, 0x84, 0x96,
	0x2e, 0x3c, 0x0a, 0x18, 0x66, 0x74, 0x42, 0x50,	0xbe, 0xac, 0x9a, 0x88, 0xf6, 0xe4, 0xd2, 0xc0,
	0x1c, 0x0e, 0x38, 0x2a, 0x54, 0x46, 0x70, 0x62,	0x8c, 0x9e, 0xa8, 0xba, 0xc4, 0xd6, 0xe0, 0xf2
};

// Calculate the CRC16 checksum for parallel 4 bit lines separately.
// When the SDIO bus operates in 4-bit mode, the CRC16 algorithm
// is applied to each line separately and generates total of
// 4 x 16 = 64 bits of checksum.
uint64_t sdio_crc16_4bit_checksum(uint32_t *data, uint32_t num_words)
{
    uint64_t crc = 0;
    uint32_t *end = data + num_words;
    while (data < end)
    {
        // Each 32-bit word contains 8 bits per line.
        // Reverse the bytes because SDIO protocol is big-endian.
        uint32_t data_in = __builtin_bswap32(*data++);

        // Shift out 8 bits for each line
        uint32_t data_out = crc >> 32;
        crc <<= 32;

        // XOR outgoing data to itself with 4 bit delay
        data_out ^= (data_out >> 16);

        // XOR incoming data to outgoing data with 4 bit delay
        data_out ^= (data_in >> 16);

        // XOR outgoing and incoming data to accumulator at each tap
        uint64_t xorred = data_out ^ data_in;
        crc ^= xorred;
        crc ^= xorred << (5 * 4);
        crc ^= xorred << (12 * 4);
    }

    return crc;
}

/*******************************************************
 * Basic SDIO command execution
 *******************************************************/

static void sdio_send_command(uint8_t command, uint32_t arg, uint8_t response_bits)
{
    // azdbg("SDIO Command: ", (int)command, " arg ", arg);

    // Format the arguments in the way expected by the PIO code.
    uint32_t word0 =
        (47 << 24) | // Number of bits in command minus one
        ( 1 << 22) | // Transfer direction from host to card
        (command << 16) | // Command byte
        (((arg >> 24) & 0xFF) << 8) | // MSB byte of argument
        (((arg >> 16) & 0xFF) << 0);
    
    uint32_t word1 =
        (((arg >> 8) & 0xFF) << 24) |
        (((arg >> 0) & 0xFF) << 16) | // LSB byte of argument
        ( 1 << 8); // End bit

    // Set number of bits in response minus one, or leave at 0 if no response expected
    if (response_bits)
    {
        word1 |= ((response_bits - 1) << 0);
    }

    // Calculate checksum in the order that the bytes will be transmitted (big-endian)
    uint8_t crc = 0;
    crc = crc7_table[crc ^ ((word0 >> 16) & 0xFF)];
    crc = crc7_table[crc ^ ((word0 >>  8) & 0xFF)];
    crc = crc7_table[crc ^ ((word0 >>  0) & 0xFF)];
    crc = crc7_table[crc ^ ((word1 >> 24) & 0xFF)];
    crc = crc7_table[crc ^ ((word1 >> 16) & 0xFF)];
    word1 |= crc << 8;
    
    // Transmit command
    pio_sm_clear_fifos(SDIO_PIO, SDIO_CMD_SM);
    pio_sm_put(SDIO_PIO, SDIO_CMD_SM, word0);
    pio_sm_put(SDIO_PIO, SDIO_CMD_SM, word1);
}

sdio_status_t rp2040_sdio_command_R1(uint8_t command, uint32_t arg, uint32_t *response)
{
    sdio_send_command(command, arg, response ? 48 : 0);

    // Wait for response
    uint32_t start = millis();
    uint32_t wait_words = response ? 2 : 1;
    while (pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_CMD_SM) < wait_words)
    {
        if ((uint32_t)(millis() - start) > 2)
        {
            azdbg("Timeout waiting for response in rp2040_sdio_command_R1(", (int)command, "), ",
                  "PIO PC: ", (int)pio_sm_get_pc(SDIO_PIO, SDIO_CMD_SM) - (int)g_sdio.pio_cmd_clk_offset,
                  " RXF: ", (int)pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_CMD_SM),
                  " TXF: ", (int)pio_sm_get_tx_fifo_level(SDIO_PIO, SDIO_CMD_SM));

            // Reset the state machine program
            pio_sm_clear_fifos(SDIO_PIO, SDIO_CMD_SM);
            pio_sm_exec(SDIO_PIO, SDIO_CMD_SM, pio_encode_jmp(g_sdio.pio_cmd_clk_offset));
            return SDIO_ERR_RESPONSE_TIMEOUT;
        }
    }

    if (response)
    {
        // Read out response packet
        uint32_t resp0 = pio_sm_get(SDIO_PIO, SDIO_CMD_SM);
        uint32_t resp1 = pio_sm_get(SDIO_PIO, SDIO_CMD_SM);
        // azdbg("SDIO R1 response: ", resp0, " ", resp1);

        // Calculate response checksum
        uint8_t crc = 0;
        crc = crc7_table[crc ^ ((resp0 >> 24) & 0xFF)];
        crc = crc7_table[crc ^ ((resp0 >> 16) & 0xFF)];
        crc = crc7_table[crc ^ ((resp0 >>  8) & 0xFF)];
        crc = crc7_table[crc ^ ((resp0 >>  0) & 0xFF)];
        crc = crc7_table[crc ^ ((resp1 >>  8) & 0xFF)];

        uint8_t actual_crc = ((resp1 >> 0) & 0xFE);
        if (crc != actual_crc)
        {
            azdbg("rp2040_sdio_command_R1(", (int)command, "): CRC error, calculated ", crc, " packet has ", actual_crc);
            return SDIO_ERR_RESPONSE_CRC;
        }

        uint8_t response_cmd = ((resp0 >> 24) & 0xFF);
        if (response_cmd != command && command != 41)
        {
            azdbg("rp2040_sdio_command_R1(", (int)command, "): received reply for ", (int)response_cmd);
            return SDIO_ERR_RESPONSE_CODE;
        }

        *response = ((resp0 & 0xFFFFFF) << 8) | ((resp1 >> 8) & 0xFF);
    }
    else
    {
        // Read out dummy marker
        pio_sm_get(SDIO_PIO, SDIO_CMD_SM);
    }

    return SDIO_OK;
}

sdio_status_t rp2040_sdio_command_R2(uint8_t command, uint32_t arg, uint8_t response[16])
{
    // The response is too long to fit in the PIO FIFO, so use DMA to receive it.
    pio_sm_clear_fifos(SDIO_PIO, SDIO_CMD_SM);
    uint32_t response_buf[5];
    dma_channel_config dmacfg = dma_channel_get_default_config(SDIO_DMA_CH);
    channel_config_set_transfer_data_size(&dmacfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dmacfg, false);
    channel_config_set_write_increment(&dmacfg, true);
    channel_config_set_dreq(&dmacfg, pio_get_dreq(SDIO_PIO, SDIO_CMD_SM, false));
    dma_channel_configure(SDIO_DMA_CH, &dmacfg, &response_buf, &SDIO_PIO->rxf[SDIO_CMD_SM], 5, true);

    sdio_send_command(command, arg, 136);

    uint32_t start = millis();
    while (dma_channel_is_busy(SDIO_DMA_CH))
    {
        if ((uint32_t)(millis() - start) > 2)
        {
            azdbg("Timeout waiting for response in rp2040_sdio_command_R2(", (int)command, "), ",
                  "PIO PC: ", (int)pio_sm_get_pc(SDIO_PIO, SDIO_CMD_SM) - (int)g_sdio.pio_cmd_clk_offset,
                  " RXF: ", (int)pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_CMD_SM),
                  " TXF: ", (int)pio_sm_get_tx_fifo_level(SDIO_PIO, SDIO_CMD_SM));

            // Reset the state machine program
            dma_channel_abort(SDIO_DMA_CH);
            pio_sm_clear_fifos(SDIO_PIO, SDIO_CMD_SM);
            pio_sm_exec(SDIO_PIO, SDIO_CMD_SM, pio_encode_jmp(g_sdio.pio_cmd_clk_offset));
            return SDIO_ERR_RESPONSE_TIMEOUT;
        }
    }

    dma_channel_abort(SDIO_DMA_CH);

    // Copy the response payload to output buffer
    response[0]  = ((response_buf[0] >> 16) & 0xFF);
    response[1]  = ((response_buf[0] >>  8) & 0xFF);
    response[2]  = ((response_buf[0] >>  0) & 0xFF);
    response[3]  = ((response_buf[1] >> 24) & 0xFF);
    response[4]  = ((response_buf[1] >> 16) & 0xFF);
    response[5]  = ((response_buf[1] >>  8) & 0xFF);
    response[6]  = ((response_buf[1] >>  0) & 0xFF);
    response[7]  = ((response_buf[2] >> 24) & 0xFF);
    response[8]  = ((response_buf[2] >> 16) & 0xFF);
    response[9]  = ((response_buf[2] >>  8) & 0xFF);
    response[10] = ((response_buf[2] >>  0) & 0xFF);
    response[11] = ((response_buf[3] >> 24) & 0xFF);
    response[12] = ((response_buf[3] >> 16) & 0xFF);
    response[13] = ((response_buf[3] >>  8) & 0xFF);
    response[14] = ((response_buf[3] >>  0) & 0xFF);
    response[15] = ((response_buf[4] >>  0) & 0xFF);

    // Calculate checksum of the payload
    uint8_t crc = 0;
    for (int i = 0; i < 15; i++)
    {
        crc = crc7_table[crc ^ response[i]];
    }

    uint8_t actual_crc = response[15] & 0xFE;
    if (crc != actual_crc)
    {
        azdbg("rp2040_sdio_command_R2(", (int)command, "): CRC error, calculated ", crc, " packet has ", actual_crc);
        return SDIO_ERR_RESPONSE_CRC;
    }

    uint8_t response_cmd = ((response_buf[0] >> 24) & 0xFF);
    if (response_cmd != 0x3F)
    {
        azdbg("rp2040_sdio_command_R2(", (int)command, "): Expected reply code 0x3F");
        return SDIO_ERR_RESPONSE_CODE;
    }

    return SDIO_OK;
}


sdio_status_t rp2040_sdio_command_R3(uint8_t command, uint32_t arg, uint32_t *response)
{
    sdio_send_command(command, arg, 48);

    // Wait for response
    uint32_t start = millis();
    while (pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_CMD_SM) < 2)
    {
        if ((uint32_t)(millis() - start) > 2)
        {
            azdbg("Timeout waiting for response in rp2040_sdio_command_R3(", (int)command, "), ",
                  "PIO PC: ", (int)pio_sm_get_pc(SDIO_PIO, SDIO_CMD_SM) - (int)g_sdio.pio_cmd_clk_offset,
                  " RXF: ", (int)pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_CMD_SM),
                  " TXF: ", (int)pio_sm_get_tx_fifo_level(SDIO_PIO, SDIO_CMD_SM));

            // Reset the state machine program
            pio_sm_clear_fifos(SDIO_PIO, SDIO_CMD_SM);
            pio_sm_exec(SDIO_PIO, SDIO_CMD_SM, pio_encode_jmp(g_sdio.pio_cmd_clk_offset));
            return SDIO_ERR_RESPONSE_TIMEOUT;
        }
    }

    // Read out response packet
    uint32_t resp0 = pio_sm_get(SDIO_PIO, SDIO_CMD_SM);
    uint32_t resp1 = pio_sm_get(SDIO_PIO, SDIO_CMD_SM);
    *response = ((resp0 & 0xFFFFFF) << 8) | ((resp1 >> 8) & 0xFF);
    // azdbg("SDIO R3 response: ", resp0, " ", resp1);

    return SDIO_OK;
}

/*******************************************************
 * Data reception from SD card
 *******************************************************/

static void sdio_start_next_block_rx()
{
    assert (g_sdio.blocks_done < g_sdio.total_blocks);

    // Disable and reset PIO from previous block
    pio_sm_set_enabled(SDIO_PIO, SDIO_DATA_SM, false);
    pio_sm_restart(SDIO_PIO, SDIO_DATA_SM);
    pio_sm_exec(SDIO_PIO, SDIO_DATA_SM, pio_encode_jmp(g_sdio.pio_data_rx_offset));

    // Start new DMA transfer
    dma_channel_transfer_to_buffer_now(SDIO_DMA_CH, g_sdio.data_buf + 128 * g_sdio.blocks_done, 128);

    // Enable PIO
    pio_sm_set_enabled(SDIO_PIO, SDIO_DATA_SM, true);
}

// Check checksums for received blocks
static void sdio_verify_rx_checksums(uint32_t maxcount)
{
    while (g_sdio.blocks_checksumed < g_sdio.blocks_done && maxcount-- > 0)
    {
        int blockidx = g_sdio.blocks_checksumed++;
        uint64_t checksum = sdio_crc16_4bit_checksum(g_sdio.data_buf + blockidx * 128, 128);
        uint64_t expected = g_sdio.block_checksums[blockidx];

        if (checksum != expected)
        {
            g_sdio.checksum_errors++;
            if (g_sdio.checksum_errors == 1)
            {
                azlog("SDIO checksum error in reception: calculated ", checksum, " expected ", expected);
            }
        }
    }
}

static void rp2040_sdio_rx_irq()
{
    dma_hw->ints1 = 1 << SDIO_DMA_CH;

    // Wait for CRC to be received
    int maxwait = 1000;
    while (pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_DATA_SM) < 2)
    {
        if (maxwait-- < 0)
        {
            azlog("rp2040_sdio_rx_irq(): timeout waiting for CRC reception");
            break;
        }
    }
    uint32_t crc0 = pio_sm_get(SDIO_PIO, SDIO_DATA_SM);
    uint32_t crc1 = pio_sm_get(SDIO_PIO, SDIO_DATA_SM);
    g_sdio.block_checksums[g_sdio.blocks_done] = ((uint64_t)crc0 << 32) | crc1;
    g_sdio.blocks_done++;

    if (g_sdio.blocks_done < g_sdio.total_blocks)
    {
        sdio_start_next_block_rx();
    }
    else
    {
        g_sdio.transfer_state = SDIO_IDLE;
    }
}

sdio_status_t rp2040_sdio_rx_start(uint8_t *buffer, uint32_t num_blocks)
{
    // Buffer must be aligned
    assert(((uint32_t)buffer & 3) == 0 && num_blocks <= SDIO_MAX_BLOCKS);

    g_sdio.transfer_state = SDIO_RX;
    g_sdio.transfer_start_time = millis();
    g_sdio.data_buf = (uint32_t*)buffer;
    g_sdio.blocks_done = 0;
    g_sdio.total_blocks = num_blocks;
    g_sdio.blocks_checksumed = 0;
    g_sdio.checksum_errors = 0;

    // Check if we are inside interrupt handler.
    // This happens when saving crash log from hardfault.
    // If true, must use polling mode instead of interrupts.
    g_sdio.inside_irq_handler = (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk);

    pio_sm_init(SDIO_PIO, SDIO_DATA_SM, g_sdio.pio_data_rx_offset, &g_sdio.pio_cfg_data_rx);
    pio_sm_set_consecutive_pindirs(SDIO_PIO, SDIO_DATA_SM, SDIO_D0, 4, false);

    // Configure DMA to receive the data block payload (512 bytes).
    dma_channel_config dmacfg = dma_channel_get_default_config(SDIO_DMA_CH);
    channel_config_set_transfer_data_size(&dmacfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dmacfg, false);
    channel_config_set_write_increment(&dmacfg, true);
    channel_config_set_dreq(&dmacfg, pio_get_dreq(SDIO_PIO, SDIO_DATA_SM, false));
    channel_config_set_bswap(&dmacfg, true);
    dma_channel_configure(SDIO_DMA_CH, &dmacfg, 0, &SDIO_PIO->rxf[SDIO_DATA_SM], 0, false);

    sdio_start_next_block_rx();

    return SDIO_OK;
}

sdio_status_t rp2040_sdio_rx_poll(uint32_t *bytes_complete)
{
    if (g_sdio.inside_irq_handler && (dma_hw->ints0 & (1 << SDIO_DMA_CH)))
    {
        // Make sure DMA interrupt handler gets called even from inside hardfault handler.
        rp2040_sdio_rx_irq();
    }

    if (bytes_complete)
    {
        *bytes_complete = g_sdio.blocks_done * 512;
    }

    if (g_sdio.transfer_state == SDIO_IDLE)
    {
        sdio_verify_rx_checksums(g_sdio.total_blocks);

        if (g_sdio.checksum_errors == 0)
            return SDIO_OK;
        else
            return SDIO_ERR_DATA_CRC;
    }
    else if ((uint32_t)(millis() - g_sdio.transfer_start_time) > 1000)
    {
        azdbg("rp2040_sdio_rx_poll() timeout, "
            "PIO PC: ", (int)pio_sm_get_pc(SDIO_PIO, SDIO_DATA_SM) - (int)g_sdio.pio_data_rx_offset,
            " RXF: ", (int)pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_DATA_SM),
            " TXF: ", (int)pio_sm_get_tx_fifo_level(SDIO_PIO, SDIO_DATA_SM),
            " DMA CNT: ", dma_hw->ch[SDIO_DMA_CH].al2_transfer_count);
        rp2040_sdio_stop();
        return SDIO_ERR_DATA_TIMEOUT;
    }
    else
    {
        // Use the idle time to calculate checksums
        sdio_verify_rx_checksums(1);
    }

    return SDIO_BUSY;
}


/*******************************************************
 * Data transmission to SD card
 *******************************************************/

static void sdio_start_next_block_tx()
{
    assert (g_sdio.blocks_done < g_sdio.total_blocks && g_sdio.blocks_checksumed > g_sdio.blocks_done);

    // Start new DMA transfer
    dma_channel_transfer_from_buffer_now(SDIO_DMA_CH, g_sdio.data_buf + 128 * g_sdio.blocks_done, 128);
}

static void sdio_compute_tx_checksums(uint32_t maxcount)
{
    while (g_sdio.blocks_checksumed < g_sdio.blocks_done && maxcount-- > 0)
    {
        int blockidx = g_sdio.blocks_checksumed++;
        g_sdio.block_checksums[blockidx] = sdio_crc16_4bit_checksum(g_sdio.data_buf + blockidx * 128, 128);
    }
}

static void rp2040_sdio_tx_irq()
{
    // Wait for there to be enough space for checksum
    int maxwait = 1000;
    while (pio_sm_get_tx_fifo_level(SDIO_PIO, SDIO_DATA_SM) < 5)
    {
        if (maxwait-- < 0)
        {
            azlog("rp2040_sdio_tx_irq(): timeout waiting for space in TX buffer for CRC");
            break;
        }
    }

    // Send the checksum and block end marker
    uint64_t crc = g_sdio.block_checksums[g_sdio.blocks_done];
    pio_sm_put(SDIO_PIO, SDIO_DATA_SM, (uint32_t)(crc >> 32));
    pio_sm_put(SDIO_PIO, SDIO_DATA_SM, (uint32_t)(crc >>  0));
    pio_sm_put(SDIO_PIO, SDIO_DATA_SM, 0xFFFFFFFF);

    g_sdio.blocks_done++;
    if (g_sdio.blocks_done < g_sdio.total_blocks)
    {
        sdio_start_next_block_tx();
    }
    else
    {
        g_sdio.transfer_state = SDIO_IDLE;
    }
}


// Start transferring data from memory to SD card
sdio_status_t rp2040_sdio_tx_start(const uint8_t *buffer, uint32_t num_blocks)
{
    // Buffer must be aligned
    assert(((uint32_t)buffer & 3) == 0 && num_blocks <= SDIO_MAX_BLOCKS);

    g_sdio.transfer_state = SDIO_TX;
    g_sdio.transfer_start_time = millis();
    g_sdio.data_buf = (uint32_t*)buffer;
    g_sdio.blocks_done = 0;
    g_sdio.total_blocks = num_blocks;
    g_sdio.blocks_checksumed = 0;
    g_sdio.checksum_errors = 0;

    // Check if we are inside interrupt handler.
    // This happens when saving crash log from hardfault.
    // If true, must use polling mode instead of interrupts.
    g_sdio.inside_irq_handler = (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk);

    // Compute first block checksum
    sdio_compute_tx_checksums(1);

    // Initialize PIO
    pio_sm_init(SDIO_PIO, SDIO_DATA_SM, g_sdio.pio_data_tx_offset, &g_sdio.pio_cfg_data_tx);
    pio_sm_set_consecutive_pindirs(SDIO_PIO, SDIO_DATA_SM, SDIO_D0, 4, true);

    // Configure DMA to send the data block payload (512 bytes)
    dma_channel_config dmacfg = dma_channel_get_default_config(SDIO_DMA_CH);
    channel_config_set_transfer_data_size(&dmacfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dmacfg, true);
    channel_config_set_write_increment(&dmacfg, false);
    channel_config_set_dreq(&dmacfg, pio_get_dreq(SDIO_PIO, SDIO_DATA_SM, false));
    channel_config_set_bswap(&dmacfg, true);
    dma_channel_configure(SDIO_DMA_CH, &dmacfg, 0, &SDIO_PIO->txf[SDIO_DATA_SM], 0, false);

    // Start first DMA transfer and PIO
    sdio_start_next_block_tx();
    pio_sm_set_enabled(SDIO_PIO, SDIO_DATA_SM, true);

    // Compute rest of the block checksums so that they are ready when needed
    sdio_compute_tx_checksums(g_sdio.total_blocks);

    return SDIO_OK;
}

// Check if transmission is complete
sdio_status_t rp2040_sdio_tx_poll(uint32_t *bytes_complete)
{
    if (g_sdio.inside_irq_handler && (dma_hw->ints0 & (1 << SDIO_DMA_CH)))
    {
        // Make sure DMA interrupt handler gets called even from inside hardfault handler.
        rp2040_sdio_tx_irq();
    }

    if (bytes_complete)
    {
        *bytes_complete = g_sdio.blocks_done * 512;
    }

    if (g_sdio.transfer_state == SDIO_IDLE)
    {
        pio_sm_set_enabled(SDIO_PIO, SDIO_DATA_SM, false);
        pio_sm_set_consecutive_pindirs(SDIO_PIO, SDIO_DATA_SM, SDIO_D0, 4, false);
        return SDIO_OK;
    }
    else if ((uint32_t)(millis() - g_sdio.transfer_start_time) > 1000)
    {
        azdbg("rp2040_sdio_tx_poll() timeout, "
            "PIO PC: ", (int)pio_sm_get_pc(SDIO_PIO, SDIO_DATA_SM) - (int)g_sdio.pio_data_rx_offset,
            " RXF: ", (int)pio_sm_get_rx_fifo_level(SDIO_PIO, SDIO_DATA_SM),
            " TXF: ", (int)pio_sm_get_tx_fifo_level(SDIO_PIO, SDIO_DATA_SM),
            " DMA CNT: ", dma_hw->ch[SDIO_DMA_CH].al2_transfer_count);
        rp2040_sdio_stop();
        return SDIO_ERR_DATA_TIMEOUT;
    }

    return SDIO_BUSY;
}

// Force everything to idle state
sdio_status_t rp2040_sdio_stop()
{
    dma_channel_abort(SDIO_DMA_CH);
    pio_sm_set_enabled(SDIO_PIO, SDIO_DATA_SM, false);
    pio_sm_set_consecutive_pindirs(SDIO_PIO, SDIO_DATA_SM, SDIO_D0, 4, false);
    g_sdio.transfer_state = SDIO_IDLE;
    return SDIO_OK;
}

void rp2040_sdio_dma_irq()
{
    dma_hw->ints1 = 1 << SDIO_DMA_CH;

    if (g_sdio.transfer_state == SDIO_TX)
        rp2040_sdio_tx_irq();
    else if (g_sdio.transfer_state == SDIO_RX)
        rp2040_sdio_rx_irq();
}

void rp2040_sdio_init()
{
    // Mark resources as being in use, unless it has been done already.
    static bool resources_claimed = false;
    if (!resources_claimed)
    {
        pio_sm_claim(SDIO_PIO, SDIO_CMD_SM);
        pio_sm_claim(SDIO_PIO, SDIO_DATA_SM);
        dma_channel_claim(SDIO_DMA_CH);
        resources_claimed = true;
    }

    memset(&g_sdio, 0, sizeof(g_sdio));

    // Load PIO programs
    pio_clear_instruction_memory(SDIO_PIO);

    // Command & clock state machine
    g_sdio.pio_cmd_clk_offset = pio_add_program(SDIO_PIO, &sdio_cmd_clk_program);
    pio_sm_config cfg = sdio_cmd_clk_program_get_default_config(g_sdio.pio_cmd_clk_offset);
    sm_config_set_out_pins(&cfg, SDIO_CMD, 1);
    sm_config_set_in_pins(&cfg, SDIO_CMD);
    sm_config_set_set_pins(&cfg, SDIO_CMD, 1);
    sm_config_set_jmp_pin(&cfg, SDIO_CMD);
    sm_config_set_sideset_pins(&cfg, SDIO_CLK);
    sm_config_set_out_shift(&cfg, false, true, 32);
    sm_config_set_in_shift(&cfg, false, true, 32);
    sm_config_set_clkdiv_int_frac(&cfg, 5, 0);
    sm_config_set_mov_status(&cfg, STATUS_TX_LESSTHAN, 2);

    pio_sm_init(SDIO_PIO, SDIO_CMD_SM, g_sdio.pio_cmd_clk_offset, &cfg);
    pio_sm_set_consecutive_pindirs(SDIO_PIO, SDIO_CMD_SM, SDIO_CLK, 1, true);
    pio_sm_set_enabled(SDIO_PIO, SDIO_CMD_SM, true);

    // Data reception program
    g_sdio.pio_data_rx_offset = pio_add_program(SDIO_PIO, &sdio_data_rx_program);
    g_sdio.pio_cfg_data_rx = sdio_data_rx_program_get_default_config(g_sdio.pio_data_rx_offset);
    sm_config_set_in_pins(&g_sdio.pio_cfg_data_rx, SDIO_D0);
    sm_config_set_in_shift(&g_sdio.pio_cfg_data_rx, false, true, 32);
    sm_config_set_fifo_join(&g_sdio.pio_cfg_data_rx, PIO_FIFO_JOIN_RX);

    // Data transmission program
    g_sdio.pio_data_tx_offset = pio_add_program(SDIO_PIO, &sdio_data_tx_program);
    g_sdio.pio_cfg_data_tx = sdio_data_rx_program_get_default_config(g_sdio.pio_data_tx_offset);
    sm_config_set_out_pins(&g_sdio.pio_cfg_data_tx, SDIO_D0, 4);
    sm_config_set_out_shift(&g_sdio.pio_cfg_data_tx, false, true, 32);
    sm_config_set_fifo_join(&g_sdio.pio_cfg_data_tx, PIO_FIFO_JOIN_TX);

    // Disable CLK pin input synchronizer.
    // This reduces delay from clk state machine to data state machine.
    // Because the CLK pin is output and driven synchronously to CPU clock,
    // there is no metastability problems.
    SDIO_PIO->input_sync_bypass |= (1 << SDIO_CLK);

    // Redirect GPIOs to PIO
    gpio_set_function(SDIO_CMD, GPIO_FUNC_PIO1);
    gpio_set_function(SDIO_CLK, GPIO_FUNC_PIO1);
    gpio_set_function(SDIO_D0, GPIO_FUNC_PIO1);
    gpio_set_function(SDIO_D1, GPIO_FUNC_PIO1);
    gpio_set_function(SDIO_D2, GPIO_FUNC_PIO1);
    gpio_set_function(SDIO_D3, GPIO_FUNC_PIO1);

    // Set up IRQ handler when DMA completes.
    // This is time-critical because the CRC must be written / read before PIO FIFO runs out.
    dma_hw->ints1 = 1 << SDIO_DMA_CH;
    dma_channel_set_irq1_enabled(SDIO_DMA_CH, true);
    irq_set_exclusive_handler(DMA_IRQ_1, rp2040_sdio_dma_irq);
    irq_set_enabled(DMA_IRQ_1, true);
    irq_set_priority(DMA_IRQ_1, 255);
}
