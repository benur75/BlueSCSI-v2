// Driver for accessing SD card in SDIO mode on RP2040.

#include "ZuluSCSI_platform.h"

#ifdef SD_USE_SDIO

#include "ZuluSCSI_log.h"
#include "rp2040_sdio.h"
#include <hardware/gpio.h>
#include <SdFat.h>
#include <SdCard/SdCardInfo.h>

static uint32_t g_sdio_ocr; // Operating condition register from card
static uint32_t g_sdio_rca; // Relative card address
static cid_t g_sdio_cid;
static int g_sdio_error_line;
static sdio_status_t g_sdio_error;

#define checkReturnOk(call) ((g_sdio_error = (call)) == SDIO_OK ? true : logSDError(__LINE__))
static bool logSDError(int line)
{
    g_sdio_error_line = line;
    azlog("SDIO SD card error on line ", line, ", error code ", (int)g_sdio_error);
    return false;
}

bool SdioCard::begin(SdioConfig sdioConfig)
{
    uint32_t reply;
    sdio_status_t status;
    
    rp2040_sdio_init();

    // Establish initial connection with the card
    for (int retries = 0; retries < 5; retries++)
    {
        delayMicroseconds(1000);
        reply = 0;
        rp2040_sdio_command_R1(CMD0, 0, NULL); // GO_IDLE_STATE
        status = rp2040_sdio_command_R1(CMD8, 0x1AA, &reply); // SEND_IF_COND

        if (status == SDIO_OK && reply == 0x1AA)
        {
            break;
        }
    }

    if (reply != 0x1AA || status != SDIO_OK)
    {
        azdbg("SDIO not responding to CMD8 SEND_IF_COND, status ", (int)status, " reply ", reply);
        return false;
    }

    // Send ACMD41 to begin card initialization and wait for it to complete
    uint32_t start = millis();
    do {
        if (!checkReturnOk(rp2040_sdio_command_R1(CMD55, 0, &reply)) || // APP_CMD
            !checkReturnOk(rp2040_sdio_command_R3(ACMD41, 0xD0040000, &g_sdio_ocr))) // 3.0V voltage
            // !checkReturnOk(rp2040_sdio_command_R1(ACMD41, 0xC0100000, &g_sdio_ocr)))
        {
            return false;
        }

        if ((uint32_t)(millis() - start) > 1000)
        {
            azlog("SDIO card initialization timeout");
            return false;
        }
    } while (!(g_sdio_ocr & (1 << 31)));

    // Get CID
    if (!checkReturnOk(rp2040_sdio_command_R2(CMD2, 0, (uint8_t*)&g_sdio_cid)))
    {
        azdbg("SDIO failed to read CID");
        return false;
    }

    // Get relative card address
    if (!checkReturnOk(rp2040_sdio_command_R1(CMD3, 0, &g_sdio_rca)))
    {
        azdbg("SDIO failed to get RCA");
        return false;
    }

    // Select card
    if (!checkReturnOk(rp2040_sdio_command_R1(CMD7, g_sdio_rca, &reply)))
    {
        azdbg("SDIO failed to select card");
        return false;
    }

    // Set 4-bit bus mode
    if (!checkReturnOk(rp2040_sdio_command_R1(CMD55, g_sdio_rca, &reply)) ||
        !checkReturnOk(rp2040_sdio_command_R1(ACMD6, 2, &reply)))
    {
        azdbg("SDIO failed to set bus width");
        return false;
    }

    return true;
}

uint8_t SdioCard::errorCode() const
{
    return g_sdio_error;
}

uint32_t SdioCard::errorData() const
{
    return 0;
}

uint32_t SdioCard::errorLine() const
{
    return g_sdio_error_line;
}

bool SdioCard::isBusy() 
{
    return (sio_hw->gpio_in & (1 << SDIO_D0)) == 0;
}

uint32_t SdioCard::kHzSdClk()
{
    return 0;
}

bool SdioCard::readCID(cid_t* cid)
{
    *cid = g_sdio_cid;
    return true;
}

bool SdioCard::readCSD(csd_t* csd)
{
    return checkReturnOk(rp2040_sdio_command_R2(CMD9, g_sdio_rca, (uint8_t*)csd)); // SEND_CSD
}

bool SdioCard::readOCR(uint32_t* ocr)
{
    // SDIO mode does not have CMD58, but main program uses this to
    // poll for card presence. Return status register instead.
    return checkReturnOk(rp2040_sdio_command_R1(CMD13, g_sdio_rca, ocr));
}

bool SdioCard::readData(uint8_t* dst)
{
    azlog("SdioCard::readData() called but not implemented!");
    return false;
}

bool SdioCard::readStart(uint32_t sector)
{
    azlog("SdioCard::readStart() called but not implemented!");
    return false;
}

bool SdioCard::readStop()
{
    azlog("SdioCard::readStop() called but not implemented!");
    return false;
}

uint32_t SdioCard::sectorCount()
{
    csd_t csd;
    readCSD(&csd);
    return sdCardCapacity(&csd);
}

uint32_t SdioCard::status()
{
    uint32_t reply;
    if (checkReturnOk(rp2040_sdio_command_R1(CMD13, g_sdio_rca, &reply)))
        return reply;
    else
        return 0;
}

bool SdioCard::stopTransmission(bool blocking)
{
    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_command_R1(CMD12, 0, &reply)))
    {
        return false;
    }

    if (!blocking)
    {
        return true;
    }
    else
    {
        uint32_t end = millis() + 100;
        while (millis() < end && isBusy())
        {
        }
        if (isBusy())
        {
            azlog("SdioCard::stopTransmission() timeout");
            return false;
        }
        else
        {
            return true;
        }
    }
}

bool SdioCard::syncDevice()
{
    return true;
}

uint8_t SdioCard::type() const
{
    if (g_sdio_ocr & (1 << 30))
        return SD_CARD_TYPE_SDHC;
    else
        return SD_CARD_TYPE_SD2;
}

bool SdioCard::writeData(const uint8_t* src)
{
    azlog("SdioCard::writeData() called but not implemented!");
    return false;
}

bool SdioCard::writeStart(uint32_t sector)
{
    azlog("SdioCard::writeStart() called but not implemented!");
    return false;
}

bool SdioCard::writeStop()
{
    azlog("SdioCard::writeStop() called but not implemented!");
    return false;
}

bool SdioCard::erase(uint32_t firstSector, uint32_t lastSector)
{
    return false;
    // return checkReturnOk(sd_erase(firstSector * 512, lastSector * 512));
}

/* Writing and reading, with progress callback */

static sd_callback_t m_stream_callback;
static const uint8_t *m_stream_buffer;
static uint32_t m_stream_count;
static uint32_t m_stream_count_start;

void azplatform_set_sd_callback(sd_callback_t func, const uint8_t *buffer)
{
    m_stream_callback = func;
    m_stream_buffer = buffer;
    m_stream_count = 0;
    m_stream_count_start = 0;
}

static sd_callback_t get_stream_callback(const uint8_t *buf, uint32_t count)
{
    m_stream_count_start = m_stream_count;

    if (m_stream_callback)
    {
        if (buf == m_stream_buffer + m_stream_count)
        {
            m_stream_count += count;
            return m_stream_callback;
        }
        else
        {
            azdbg("Stream buffer mismatch: ", (uint32_t)buf, " vs. ", (uint32_t)(m_stream_buffer + m_stream_count));
            return NULL;
        }
    }
    
    return NULL;
}


bool SdioCard::writeSector(uint32_t sector, const uint8_t* src)
{
    sd_callback_t callback = get_stream_callback(src, 512);

    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_command_R1(16, 512, &reply)) || // SET_BLOCKLEN
        !checkReturnOk(rp2040_sdio_command_R1(CMD24, sector, &reply)) || // WRITE_BLOCK
        !checkReturnOk(rp2040_sdio_tx_start(src, 1))) // Start transmission
    {
        return false;
    }

    do {
        uint32_t bytes_done;
        g_sdio_error = rp2040_sdio_tx_poll(&bytes_done);

        if (callback)
        {
            callback(m_stream_count_start + bytes_done);
        }
    } while (g_sdio_error == SDIO_BUSY);

    if (g_sdio_error != SDIO_OK)
    {
        azdbg("SdioCard::writeSector(", sector, ") failed: ", (int)g_sdio_error);
    }

    return g_sdio_error == SDIO_OK;
}

bool SdioCard::writeSectors(uint32_t sector, const uint8_t* src, size_t n)
{
    sd_callback_t callback = get_stream_callback(src, 512);

    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_command_R1(16, 512, &reply)) || // SET_BLOCKLEN
        !checkReturnOk(rp2040_sdio_command_R1(CMD55, g_sdio_rca, &reply)) || // APP_CMD
        !checkReturnOk(rp2040_sdio_command_R1(ACMD23, n, &reply)) || // SET_WR_CLK_ERASE_COUNT
        !checkReturnOk(rp2040_sdio_command_R1(CMD25, sector, &reply)) || // WRITE_MULTIPLE_BLOCK
        !checkReturnOk(rp2040_sdio_tx_start(src, n))) // Start transmission
    {
        return false;
    }

    do {
        uint32_t bytes_done;
        g_sdio_error = rp2040_sdio_tx_poll(&bytes_done);

        if (callback)
        {
            callback(m_stream_count_start + bytes_done);
        }
    } while (g_sdio_error == SDIO_BUSY);

    checkReturnOk(rp2040_sdio_command_R1(CMD12, 0, &reply)); // STOP_TRANSMISSION

    if (g_sdio_error != SDIO_OK)
    {
        azdbg("SdioCard::writeSectors(", sector, ",...,", (int)n, ") failed: ", (int)g_sdio_error);
    }

    return g_sdio_error == SDIO_OK;
}

bool SdioCard::readSector(uint32_t sector, uint8_t* dst)
{
    sd_callback_t callback = get_stream_callback(dst, 512);

    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_command_R1(16, 512, &reply)) || // SET_BLOCKLEN
        !checkReturnOk(rp2040_sdio_rx_start(dst, 1)) || // Prepare for reception
        !checkReturnOk(rp2040_sdio_command_R1(CMD17, sector, &reply))) // READ_SINGLE_BLOCK
    {
        return false;
    }

    do {
        uint32_t bytes_done;
        g_sdio_error = rp2040_sdio_rx_poll(&bytes_done);

        if (callback)
        {
            callback(m_stream_count_start + bytes_done);
        }
    } while (g_sdio_error == SDIO_BUSY);

    if (g_sdio_error != SDIO_OK)
    {
        azdbg("SdioCard::readSector(", sector, ") failed: ", (int)g_sdio_error);
    }

    return g_sdio_error == SDIO_OK;
}

bool SdioCard::readSectors(uint32_t sector, uint8_t* dst, size_t n)
{
    sd_callback_t callback = get_stream_callback(dst, n * 512);

    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_command_R1(16, 512, &reply)) || // SET_BLOCKLEN
        !checkReturnOk(rp2040_sdio_rx_start(dst, n)) || // Prepare for reception
        !checkReturnOk(rp2040_sdio_command_R1(CMD18, sector, &reply))) // READ_MULTIPLE_BLOCK
    {
        return false;
    }

    do {
        uint32_t bytes_done;
        g_sdio_error = rp2040_sdio_rx_poll(&bytes_done);

        if (callback)
        {
            callback(m_stream_count_start + bytes_done);
        }
    } while (g_sdio_error == SDIO_BUSY);

    checkReturnOk(rp2040_sdio_command_R1(CMD12, 0, &reply)); // STOP_TRANSMISSION

    if (g_sdio_error != SDIO_OK)
    {
        azdbg("SdioCard::readSectors(", sector, ",...,", (int)n, ") failed: ", (int)g_sdio_error);
    }

    return g_sdio_error == SDIO_OK;
}

// These functions are not used for SDIO mode but are needed to avoid build error.
void sdCsInit(SdCsPin_t pin) {}
void sdCsWrite(SdCsPin_t pin, bool level) {}

// SDIO configuration for main program
SdioConfig g_sd_sdio_config(DMA_SDIO);

#endif