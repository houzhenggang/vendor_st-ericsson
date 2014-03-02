/*
 * Copyright (C) ST-Ericsson SA 2012. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <debug.h>
#include <reg.h>
#include <platform/timer.h>		/* udelay() */
#include <kernel/thread.h>

#include "mmc_host.h"
#include "mmc_types.h"
#include "mmc_fifo.h"
#include "mmc_if.h"
#include "target.h"
#include "machineid.h"


/*
 * wait_for_command_end() - waiting for the command completion
 * this function will wait until the command completion has happened or
 * any error generated by reading the status register
 */
static int wait_for_command_end(mmc_properties_t *dev, mmc_cmd_t *cmd)
{
	uint32_t hoststatus, statusmask;
	mmc_host_t *host = dev->host;

	statusmask = SDI_STA_CTIMEOUT | SDI_STA_CCRCFAIL;
	if ((cmd->resp > MC_RESP_NONE))
		statusmask |= SDI_STA_CMDREND;
	else
		statusmask |= SDI_STA_CMDSENT;

	do
		hoststatus = readl(&host->base->status) & statusmask;
	while (!hoststatus);

	dprintf(SPEW, "wait_for_command_end: SDI_ICR <= 0x%08X\n", statusmask);
	writel(statusmask, &host->base->status_clear);

	if (hoststatus & SDI_STA_CTIMEOUT) {
		dprintf(SPEW, "CMD%d time out\n", cmd->id);
		return MMC_TIMEOUT;
	} else if ((hoststatus & SDI_STA_CCRCFAIL) &&
		   (cmd->rsp_crc)) {
		dprintf(SPEW, "CMD%d CRC error\n", cmd->id);
		return MMC_CMD_CRC_FAIL;
	}

	if (cmd->resp > MC_RESP_NONE) {
		cmd->resp_data[0] = readl(&host->base->response0);
		cmd->resp_data[1] = readl(&host->base->response1);
		cmd->resp_data[2] = readl(&host->base->response2);
		cmd->resp_data[3] = readl(&host->base->response3);
		dprintf(SPEW,
			"CMD%d response[0]:0x%08X, response[1]:0x%08X, "
			"response[2]:0x%08X, response[3]:0x%08X\n",
			cmd->id, cmd->resp_data[0], cmd->resp_data[1],
			cmd->resp_data[2], cmd->resp_data[3]);
	}
	return MMC_OK;
}

/*
 * do_command - sends command to card, and waits for its result.
 */
static int do_command(mmc_properties_t *dev, mmc_cmd_t *cmd)
{
	int result;
	uint32_t sdi_cmd;
	mmc_host_t *host = dev->host;
	uint32_t statusmask;

	statusmask = 0xFFFF;
	dprintf(SPEW, "do_command: SDI_ICR <= 0x%08X\n", statusmask);
	writel(statusmask, &host->base->status_clear);

	dprintf(SPEW, "Request to do CMD%d, CRC %d\n", cmd->id, cmd->rsp_crc);

	sdi_cmd = (cmd->id & SDI_CMD_CMDINDEX_MASK) | SDI_CMD_CPSMEN;

	if (cmd->resp) {
		sdi_cmd |= SDI_CMD_WAITRESP;
		if (cmd->rsp_long)
			sdi_cmd |= SDI_CMD_LONGRESP;
	}

	dprintf(SPEW, "SDI_ARG <= 0x%08X\n", cmd->arg);
	writel((uint32_t)cmd->arg, &host->base->argument);

	/*
	 * Note: After a data write, data cannot be written to the cmd register
	 * for three MCLK (Clock that drives the SDI logic) clock periods plus
	 * two PCLK (Peripheral bus clock) clock periods.
	 */
	udelay(COMMAND_REG_DELAY); /* DONT REMOVE */
	dprintf(SPEW, "SDI_CMD <= 0x%08X\n", sdi_cmd);

	writel(sdi_cmd, &host->base->command);
	result = wait_for_command_end(dev, cmd);
	dprintf(SPEW, "do_command: wait_for_command_end returned %d\n", result);

	/* After CMD2 set RCA to a none zero value. */
	if ((result == MMC_OK) && (cmd->id == MC_ALL_SEND_CID))
		dev->rca = 10;

	/* After CMD3 open drain is switched off and push pull is used. */
	if ((result == MMC_OK) && (cmd->id == MC_SET_RELATIVE_ADDR)) {
		uint32_t sdi_pwr = readl(&host->base->power) & ~SDI_PWR_OPD;
		dprintf(SPEW, "SDI_PWR <= 0x%08X\n", sdi_pwr);
		writel(sdi_pwr, &host->base->power);
	}

	return result;
}

#if 0
static int convert_from_bytes_to_power_of_two(unsigned int x)
{
	int y = 0;
	y = (x & 0xAAAA) ? 1 : 0;
	y |= ((x & 0xCCCC) ? 1 : 0)<<1;
	y |= ((x & 0xF0F0) ? 1 : 0)<<2;
	y |= ((x & 0xFF00) ? 1 : 0)<<3;

	return y;
}
#endif

/*
 * read_bytes - reads bytes from the card, part of data transfer.
 */
static int read_bytes(mmc_properties_t *dev, uint32_t *dest, uint32_t blkcount, uint32_t blksize)
{
	uint64_t xfercount = blkcount * blksize;
	mmc_host_t *host = dev->host;
	uint32_t status;

	dprintf(SPEW, "read_bytes: blkcount=%u blksize=%u, dest 0x%lx\n",
		blkcount, blksize, (unsigned long) dest);

	status = mmc_fifo_read(&host->base->fifo, dest, xfercount,
				&host->base->status);

	if (status & (SDI_STA_DTIMEOUT | SDI_STA_DCRCFAIL)) {
		printf("Reading data failed: status:0x%08X\n", status);
		if (status & SDI_STA_DTIMEOUT)
			return MMC_DATA_TIMEOUT;
		else if (status & SDI_STA_DCRCFAIL)
			return MMC_DATA_CRC_FAIL;
	}

	dprintf(SPEW, "SDI_ICR <= 0x%08X\n", SDI_ICR_MASK);
	writel(SDI_ICR_MASK, &host->base->status_clear);
	dprintf(SPEW, "Reading data completed status:0x%08X\n",
		status);

	return MMC_OK;
}

/*
 * write_bytes - writes byte to the card, part of data transfer.
 */
static int write_bytes(mmc_properties_t *dev, uint32_t *src, uint32_t blkcount, uint32_t blksize)
{
	uint64_t xfercount = blkcount * blksize;
	mmc_host_t *host = dev->host;
	uint32_t status;

	dprintf(SPEW, "write_bytes: blkcount=%u blksize=%u\n",
		blkcount, blksize);

	status = mmc_fifo_write(src, &host->base->fifo, xfercount,
				   &host->base->status);

	if (status & (SDI_STA_DTIMEOUT | SDI_STA_DCRCFAIL)) {
		printf("Writing data failed: status=0x%08X\n", status);
		if (status & SDI_STA_DTIMEOUT)
			return MMC_DATA_TIMEOUT;
		else if (status & SDI_STA_DCRCFAIL)
			return MMC_DATA_CRC_FAIL;
	}

	writel(SDI_ICR_MASK, &host->base->status_clear);
	dprintf(SPEW, "Writing data completed status:0x%08X\n",
		status);

	return MMC_OK;
}

/*
 * do_data_transfer - for doing any data transfer operation.
 *
 * dev: mmc device for doing the operation on.
 * cmd: cmd to do.
 * data: if cmd warrants any data transfer.
 */
static int do_data_transfer(mmc_properties_t *dev,
			    mmc_cmd_t *cmd,
			    mmc_data_t *data)
{
	int error = MMC_DATA_TIMEOUT;
	mmc_host_t *host = dev->host;
	uint32_t blksz = 0;
	uint32_t data_ctrl = 0;
	uint32_t data_len = (uint32_t) (data->n_blocks * data->block_size);

	dprintf(SPEW, "Request to do data xfer\n");
	dprintf(SPEW, "do_data_transfer(%u) start\n", data->n_blocks);

	blksz = data->block_size;
	data_ctrl |= (blksz << INDEX(SDI_DCTRL_DBLOCKSIZE_V2_MASK));

	data_ctrl |= SDI_DCTRL_DTEN | SDI_DCTRL_BUSYMODE;

	if ((dev->ddr == DDR_ENABLED) &&
		((cmd->id == MC_READ_SINGLE_BLOCK)	||
		 (cmd->id == MC_READ_MULTIPLE_BLOCK)	||
		 (cmd->id == MC_WRITE_SINGLE_BLOCK)	||
		 (cmd->id == MC_WRITE_MULTIPLE_BLOCK)	||
		 (cmd->id == MC_SEND_EXT_CSD)))
		data_ctrl |= SDI_DCTRL_DDR_MODE;

	dprintf(SPEW, "SDI_DTIMER <= 0x%08X\n", dev->data_timeout);
	writel(dev->data_timeout, &host->base->datatimer);
	dprintf(SPEW, "SDI_DLEN <= 0x%08X\n", data_len);
	writel(data_len, &host->base->datalength);

	udelay(DATA_REG_DELAY); /* DONT REMOVE */

	/* Wait if busy */
	while (readl(&host->base->status) & SDI_STA_CARDBUSY);

	if (data->rd_wr == DATA_READ) {
		dprintf(SPEW, "It is a read operation\n");

		data_ctrl |= SDI_DCTRL_DTDIR_IN;
		dprintf(SPEW, "SDI_DCTRL <= 0x%08X\n", data_ctrl);
		writel(data_ctrl, &host->base->datactrl);

		error = do_command(dev, cmd);
		if (error)
			return error;

		error = read_bytes(dev,
				   (uint32_t *)data->data,
				   (uint32_t)data->n_blocks,
				   (uint32_t)data->block_size);
	} else if (data->rd_wr == DATA_WRITE) {
		dprintf(SPEW, "It is a write operation\n");

		error = do_command(dev, cmd);
		if (error)
			return error;

		dprintf(SPEW, "SDI_DCTRL <= 0x%08X\n", data_ctrl);
		writel(data_ctrl, &host->base->datactrl);

		error = write_bytes(dev,
				    (uint32_t *)data->data,
				    (uint32_t)data->n_blocks,
				    (uint32_t)data->block_size);
	}

	dprintf(SPEW, "do_data_transfer() end\n");

	return error;
}

/*
 * host_request - For all operations on cards.
 *
 * dev: mmc device for doing the operation on.
 * cmd: cmd to do.
 * data: if cmd warrants any data transfer.
 */
//static int host_request(struct mmc *dev,
int host_request(mmc_properties_t *dev,
			mmc_cmd_t *cmd,
			mmc_data_t *data)
{
	int result;

	enter_critical_section();
	if (data)
		result = do_data_transfer(dev, cmd, data);
	else
		result = do_command(dev, cmd);
	exit_critical_section();

	return result;
}

/*
 * This is to initialize card specific things just before enumerating
 * them. MMC cards uses open drain drivers in enumeration phase.
 */
//static int mmc_host_reset(struct mmc *dev)
int mmc_host_reset(mmc_properties_t *dev)
{
	mmc_host_t *host = dev->host;
	uint32_t sdi_u32 = SDI_PWR_OPD | SDI_PWR_PWRCTRL_ON;

	dprintf(SPEW, "SDI_PWR <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->power);

#if PLATFORM_DB8540 && CONFIG_SDMMC_NEW_CLKPATH
	sdi_u32 = SDI_CLKCR_CLKDIV_INIT | SDI_CLKCR_CLKEN | SDI_CLKCR_HWFC_EN
				| SDI_CLKCR_DIV2;
#else
	sdi_u32 = SDI_CLKCR_CLKDIV_INIT | SDI_CLKCR_CLKEN | SDI_CLKCR_HWFC_EN;
#endif

	dprintf(SPEW, "SDI_CLKCR <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->clock);

	udelay(CLK_CHANGE_DELAY);

	return MMC_OK;
}

/*
 * This is to initialize card specific things just before enumerating
 * them. SD cards do not need to be initialized.
 */
//static int sd_host_reset(struct mmc *dev)
int sd_host_reset(mmc_properties_t *dev)
{
	(void) dev;  /* Parameter not used! */

	return MMC_OK;
}

/*
 * host_set_ios:to configure host parameters.
 *
 * dev: the pointer to the host structure for MMC.
 */
//static void host_set_ios(struct mmc *dev)
void host_set_ios(mmc_properties_t *dev)
{
	mmc_host_t *host = dev->host;
	uint32_t sdi_clkcr;
	uint32_t buswidth = 0;
	uint32_t clkdiv = 0;
	uint32_t tmp_clock;

	dprintf (SPEW, "set_ios: clk %d, width 0x%08x\n", dev->device_clock, dev->bus_width);

	/* First read out the contents of clock control register. */
	sdi_clkcr = readl(&host->base->clock);

	/* Set the clock rate and bus width */
	if (dev->device_clock) {
		dprintf(SPEW,
			"setting clock and bus width in the host: device_clock %d MCLK %d, bus %d ", dev->device_clock, MCLK, dev->bus_width);
		/* Check if clock bypass is needed */
#if PLATFORM_DB8540
#if CONFIG_SDMMC_NEW_CLKPATH
		if (dev->device_clock >= MCLK / 2) {
			sdi_clkcr |= SDI_CLKCR_BYPASS | SDI_CLKCR_NEGEDGE | SDI_CLKCR_DIV2;
			clkdiv = 0;
			dev->device_clock = MCLK / 2;
		} else {
			sdi_clkcr &= ~(SDI_CLKCR_BYPASS | SDI_CLKCR_NEGEDGE);
			sdi_clkcr |= SDI_CLKCR_DIV2;
			clkdiv = (MCLK / (2 * dev->device_clock))- 2;
			tmp_clock = MCLK / (2 * (clkdiv + 2));
			while (tmp_clock > dev->device_clock) {
				clkdiv++;
				tmp_clock = MCLK / (2 * (clkdiv + 2));
			}
			if (clkdiv > (SDI_CLKCR_CLKDIV_MASK >> 17))
				clkdiv = SDI_CLKCR_CLKDIV_MASK >> 17;
			tmp_clock = MCLK / (2 * (clkdiv + 2));
			dev->device_clock = tmp_clock;
		}
		sdi_clkcr &= ~(SDI_CLKCR_CLKDIV_MASK);
		sdi_clkcr |= clkdiv << 17;
#else
		if (dev->device_clock >= MCLK) {
			sdi_clkcr |= SDI_CLKCR_BYPASS | SDI_CLKCR_NEGEDGE;
			clkdiv = 0;
			dev->device_clock = MCLK;
		} else {
			sdi_clkcr &= ~(SDI_CLKCR_BYPASS | SDI_CLKCR_NEGEDGE);
			clkdiv = (MCLK / dev->device_clock)- 2;
			tmp_clock = MCLK / (clkdiv + 2);
			while (tmp_clock > dev->device_clock) {
				clkdiv++;
				tmp_clock = MCLK / (clkdiv + 2);
			}
			if (clkdiv > (SDI_CLKCR_CLKDIV_MASK >> 17))
				clkdiv = SDI_CLKCR_CLKDIV_MASK >> 17;
			tmp_clock = MCLK / (clkdiv + 2);
			dev->device_clock = tmp_clock;
		}
		sdi_clkcr &= ~SDI_CLKCR_CLKDIV_MASK;
		sdi_clkcr |= clkdiv << 17;
#endif
#else
		if (dev->device_clock >= MCLK){
			sdi_clkcr |= SDI_CLKCR_BYPASS | SDI_CLKCR_NEGEDGE;
			clkdiv = 0;
			dev->device_clock = MCLK;
		} else {
			sdi_clkcr &= ~(SDI_CLKCR_BYPASS | SDI_CLKCR_NEGEDGE);
			clkdiv = (MCLK / dev->device_clock) - 2;
		}
		tmp_clock = MCLK / (clkdiv + 2);
		while (tmp_clock > dev->device_clock) {
			clkdiv++;
			tmp_clock = MCLK / (clkdiv + 2);
		}
		if (clkdiv > SDI_CLKCR_CLKDIV_MASK)
			clkdiv = SDI_CLKCR_CLKDIV_MASK;
		tmp_clock = MCLK / (clkdiv + 2);
		dev->device_clock = tmp_clock;
		sdi_clkcr &= ~(SDI_CLKCR_CLKDIV_MASK);
		sdi_clkcr |= clkdiv;
#endif
		dprintf(SPEW,"post clk %d\n", dev->device_clock);
	}

	if (dev->bus_width) {
		switch (dev->bus_width) {
		case BUS_WIDTH_1:
			buswidth |= SDI_CLKCR_WIDBUS_1;
			break;
		case BUS_WIDTH_4:
			buswidth |= SDI_CLKCR_WIDBUS_4;
			break;
		case BUS_WIDTH_8:
			buswidth |= SDI_CLKCR_WIDBUS_8;
			break;
		default:
			printf("wrong bus width, so ignoring");
			break;
		}
		sdi_clkcr &= ~(SDI_CLKCR_WIDBUS_MASK);
		sdi_clkcr |= buswidth;
		dprintf(SPEW,"set_ios bus 0x%08x, SDI_CLKCR_WIDBUS_8 0x%08x\n", buswidth, SDI_CLKCR_WIDBUS_8);
	}

	dprintf (SPEW, "set_ios: buswidth 0x%08x, clkdiv 0x%08x\n", buswidth, clkdiv);

	dev->data_timeout = MMC_DATA_TIMEOUT * dev->device_clock;

	dprintf(SPEW, "SDI_CLKCR <= 0x%08X\n", sdi_clkcr);
	writel(sdi_clkcr, &host->base->clock);

	udelay(CLK_CHANGE_DELAY);

	dprintf(SPEW, "host_set_ios: done\n");
}

mmc_properties_t *u8500_alloc_mmc_struct(void)
{
	mmc_host_t *host = NULL;
	mmc_properties_t *mmc_device = NULL;

	host = malloc(sizeof(mmc_host_t));
	if (!host)
		return NULL;

	mmc_device = malloc(sizeof(mmc_properties_t));
	if (!mmc_device)
		goto err;

	memset(mmc_device, 0x00, sizeof(mmc_properties_t));

	mmc_device->host = host;
	return mmc_device;
 err:
	free(host);
	return NULL;
}

/*
 * emmc_host_init - initialize the emmc controller.
 * Configure GPIO settings, set initial clock and power for emmc slot.
 * Initialize mmc struct and register with mmc framework.
 */
int u8500_emmc_host_init(mmc_properties_t *dev, struct sdi_registers *base)
{
	mmc_host_t *host = dev->host;
	uint32_t sdi_u32;

	host->base = base;
	sdi_u32 = SDI_PWR_OPD | SDI_PWR_PWRCTRL_ON;
	dprintf(SPEW, "u8500_emmc_host_init: SDI_PWR <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->power);

	/* setting clk freq less than 400KHz */
#if PLATFORM_DB8540 && CONFIG_SDMMC_NEW_CLKPATH
	sdi_u32 = SDI_CLKCR_CLKDIV_INIT | SDI_CLKCR_CLKEN | SDI_CLKCR_HWFC_EN
				| SDI_CLKCR_DIV2;
#else
	sdi_u32 = SDI_CLKCR_CLKDIV_INIT | SDI_CLKCR_CLKEN | SDI_CLKCR_HWFC_EN;
#endif
	dprintf(SPEW, "u8500_emmc_host_init: SDI_CLKCR <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->clock);

	udelay(CLK_CHANGE_DELAY);

	sdi_u32 = readl(&host->base->mask0) & ~SDI_MASK0_MASK;
	dprintf(SPEW, "u8500_emmc_host_init: SDI_MASK0 <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->mask0);

#if PLATFORM_DB8540
#if CONFIG_SDMMC_NEW_CLKPATH
	dev->host->host_clock = MCLK / (2 * (2 + (SDI_CLKCR_CLKDIV_INIT >> 17)));
#else
	dev->host->host_clock = MCLK / (2 + (SDI_CLKCR_CLKDIV_INIT >> 17));
#endif
#else
	dev->host->host_clock = MCLK / (2 + SDI_CLKCR_CLKDIV_INIT);
#endif

	dev->host->host_bus_width = BUS_WIDTH_4 | BUS_WIDTH_8;
	dev->host->host_hs_timing = HS_TIMING_SET;

	dev->voltage_window = VOLTAGE_WINDOW_MMC;
	dev->rel_write = REL_WRITE_FALSE;
	dev->host->host_blocksize = SDI_DCTRL_DBLOCKSIZE_V2_MASK >> 16;
	dev->ddr = DDR_DISABLED;

	return 0;
}

/*
 * mmc_host_init - initialize the external mmc controller.
 * Configure GPIO settings, set initial clock and power for mmc slot.
 * Initialize mmc struct and register with mmc framework.
 */
int u8500_mmc_host_init(mmc_properties_t *dev, struct sdi_registers *base)
{
	mmc_host_t *host = dev->host;
	uint32_t sdi_u32;

	host->base = base;
	sdi_u32 = 0xBF;
	dprintf(SPEW, "SDI_PWR <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->power);
	/* setting clk freq just less than 400KHz */
#if PLATFORM_DB8540 && CONFIG_SDMMC_NEW_CLKPATH
	sdi_u32 = SDI_CLKCR_CLKDIV_INIT | SDI_CLKCR_CLKEN | SDI_CLKCR_HWFC_EN
				| SDI_CLKCR_DIV2;
#else
	sdi_u32 = SDI_CLKCR_CLKDIV_INIT | SDI_CLKCR_CLKEN | SDI_CLKCR_HWFC_EN;
#endif
	dprintf(SPEW, "SDI_CLKCR <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->clock);

	udelay(CLK_CHANGE_DELAY);

	sdi_u32 = readl(&host->base->mask0) & ~SDI_MASK0_MASK;
	dprintf(SPEW, "SDI_MASK0 <= 0x%08X\n", sdi_u32);
	writel(sdi_u32, &host->base->mask0);

#if PLATFORM_DB8540
#if CONFIG_SDMMC_NEW_CLKPATH
	dev->host->host_clock = MCLK / (2 * (2 + (SDI_CLKCR_CLKDIV_INIT >> 17)));
#else
	dev->host->host_clock = MCLK / (2 + (SDI_CLKCR_CLKDIV_INIT >> 17));
#endif
#else
	dev->host->host_clock = MCLK / (2 + SDI_CLKCR_CLKDIV_INIT);
#endif

	dev->host->host_bus_width = BUS_WIDTH_4;
	dev->host->host_hs_timing = HS_TIMING_SET;

	dev->voltage_window = VOLTAGE_WINDOW_SD;
	dev->rel_write = REL_WRITE_FALSE;
	dev->host->host_blocksize = SDI_DCTRL_DBLOCKSIZE_V2_MASK >> 16;
	dev->ddr = DDR_DISABLED;

	return 0;
}


void u8500_mmc_host_printregs(mmc_properties_t *dev)
{
	uint32_t sdi;
	mmc_host_t *host = dev->host;

	sdi = readl(&host->base->power);		/* 0x00 */
	dprintf (SPEW, "host->base->power      0x%08x\n", sdi);
	sdi = readl(&host->base->clock);		/* 0x04 */
	dprintf (SPEW, "host->base->clock      0x%08x\n", sdi);
}
