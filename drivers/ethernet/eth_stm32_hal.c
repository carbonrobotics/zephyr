/*
 * Copyright (c) 2017 Erwin Rol <erwin@erwinrol.com>
 * Copyright (c) 2020 Alexander Kozhinov <AlexanderKozhinov@yandex.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_ethernet

#define LOG_MODULE_NAME eth_stm32_hal
#define LOG_LEVEL CONFIG_ETHERNET_LOG_LEVEL

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <kernel.h>
#include <device.h>
#include <sys/__assert.h>
#include <sys/util.h>
#include <errno.h>
#include <stdbool.h>
#include <net/net_pkt.h>
#include <net/net_if.h>
#include <net/ethernet.h>
#include <ethernet/eth_stats.h>
#include <soc.h>
#include <sys/printk.h>
#include <drivers/clock_control.h>
#include <drivers/clock_control/stm32_clock_control.h>
#include <pinmux/pinmux_stm32.h>

#if defined(CONFIG_PTP_CLOCK_STM32)
#include <drivers/ptp_clock.h>
#include <net/gptp.h>
#endif

#include "eth.h"
#include "eth_stm32_hal_priv.h"

#if defined(CONFIG_ETH_STM32_HAL_USE_DTCM_FOR_DMA_BUFFER) &&                                       \
	!DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_dtcm), okay)
#error DTCM for DMA buffer is activated but zephyr,dtcm is not present in dts
#endif

#define PHY_ADDR CONFIG_ETH_STM32_HAL_PHY_ADDRESS

#if defined(CONFIG_SOC_SERIES_STM32H7X)

#define PHY_BSR ((uint16_t)0x0001U) /*!< Transceiver Basic Status Register */
#define PHY_LINKED_STATUS ((uint16_t)0x0004U) /*!< Valid link established */

#define GET_FIRST_DMA_TX_DESC(heth) (heth->Init.TxDesc)
#define IS_ETH_DMATXDESC_OWN(dma_tx_desc) (dma_tx_desc->DESC3 & ETH_DMATXNDESCRF_OWN)

#define ETH_RXBUFNB ETH_RX_DESC_CNT
#define ETH_TXBUFNB ETH_TX_DESC_CNT

#define ETH_MEDIA_INTERFACE_MII HAL_ETH_MII_MODE
#define ETH_MEDIA_INTERFACE_RMII HAL_ETH_RMII_MODE

#define ETH_DMA_TX_TIMEOUT_MS 20U /* transmit timeout in milliseconds */

/* Only one tx_buffer is sufficient to pass only 1 dma_buffer */
#define ETH_TXBUF_DEF_NB 1U
#else

#define GET_FIRST_DMA_TX_DESC(heth) (heth->TxDesc)
#define IS_ETH_DMATXDESC_OWN(dma_tx_desc) (dma_tx_desc->Status & ETH_DMATXDESC_OWN)

#endif /* CONFIG_SOC_SERIES_STM32H7X */

#if defined(CONFIG_ETH_STM32_HAL_USE_DTCM_FOR_DMA_BUFFER) &&                                       \
	DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_dtcm), okay)
#define __eth_stm32_desc __dtcm_noinit_section
#define __eth_stm32_buf __dtcm_noinit_section
#elif defined(CONFIG_SOC_SERIES_STM32H7X) && DT_NODE_HAS_STATUS(DT_NODELABEL(sram3), okay)
#define __eth_stm32_desc __attribute__((section(".eth_stm32_desc")))
#define __eth_stm32_buf __attribute__((section(".eth_stm32_buf")))
#elif defined(CONFIG_NOCACHE_MEMORY)
#define __eth_stm32_desc __nocache __aligned(4)
#define __eth_stm32_buf __nocache __aligned(4)
#else
#define __eth_stm32_desc __aligned(4)
#define __eth_stm32_buf __aligned(4)
#endif

static ETH_DMADescTypeDef dma_rx_desc_tab[ETH_RXBUFNB] __eth_stm32_desc;
static ETH_DMADescTypeDef dma_tx_desc_tab[ETH_TXBUFNB] __eth_stm32_desc;
static uint8_t dma_rx_buffer[ETH_RXBUFNB][ETH_RX_BUF_SIZE] __eth_stm32_buf;
static uint8_t dma_tx_buffer[ETH_TXBUFNB][ETH_TX_BUF_SIZE] __eth_stm32_buf;

#if defined(CONFIG_SOC_SERIES_STM32H7X)
static ETH_TxPacketConfig tx_config;

void enablePTP(ETH_HandleTypeDef *heth)
{
	LOG_INF("inside enable PTP");

	// 1. Mask the Time stamp trigger interrupt by clearing bit 16 in the MACIER register.
	heth->Instance->MACIER &= ~(ETH_MACIER_TSIE);

	while ((heth->Instance->MACIER & ETH_MACIER_TSIE_Msk) >> ETH_MACIER_TSIE_Pos != 0)
		;

	// 2. Set Time stamp register bit 0 to enable time stamping.
	heth->Instance->MACTSCR |= 0x01;

	while ((heth->Instance->MACTSCR & 0x01) != 1)
		;

	heth->Instance->MACTSCR |= (ETH_MACTSCR_TSIPV4ENA | ETH_MACTSCR_TSVER2ENA |
				    ETH_MACTSCR_TSENALL | ETH_MACTSCR_TSCTRLSSR);

	// 3. Program the Subsecond increment register based on the PTP clock
	// frequency. ours is a 100 mHz clock as currently configured, so 10 ns period,

	heth->Instance->MACSSIR =
		0x000A0000; // 0x00160000; //0x000B0000;  // subsecond increment register

	// addend register. this number keeps getting added to an accumulator,
	// once accumulator overflow, subsecond register gets incremented by ssir
	// amount

	// 4. program the Time stamp addend register and set Time stamp control
	// register bit 5 (addend register update).

	// 200 / 50 = 4 [50 Mhz required for 20 ns accuracy], 2^32 / 4 =
	// 994,205,392.5925926
	heth->Instance->MACTSAR = 0xFFFFFFFF; //1073741824; // 2147483648
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSADDREG; // set bit 5 to update addend register.

	// 5. Wait for TSCR bit 5 to be cleared
	while ((heth->Instance->MACTSCR & 0x20) >> 5 != 0) {
	}

	// 6. To select the Fine correction method (if required), program Time stamp
	// control register bit 1.
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSCFUPDT;

	while ((heth->Instance->MACTSCR & ETH_MACTSCR_TSCFUPDT) >> ETH_MACTSCR_TSCFUPDT_Pos == 0) {
	}

	// 7. Initialize the system time by programming the Time stamp high update and
	// Time stamp low update registers with the appropriate time value.

	heth->Instance->MACSTSUR = 1600000000; // ~50 years in seconds. (epoch time? roughly)
	heth->Instance->MACSTNUR = 1;

	// 8. Set Time stamp control register bit 2 (Time stamp init).
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSINIT;

	while ((heth->Instance->MACTSCR & ETH_MACTSCR_TSINIT) >> ETH_MACTSCR_TSINIT_Pos == 0) {
	}

	// 9. Enable the MAC receiver and transmitter for proper time stamping.
	heth->Instance->MACCR |= (ETH_MACCR_TE | ETH_MACCR_RE);

	while ((heth->Instance->MACCR & 0x03) != 3) {
	}
}

#endif

#if defined(CONFIG_NET_L2_CANBUS_ETH_TRANSLATOR)
#include <net/can.h>

static void set_mac_to_translator_addr(uint8_t *mac_addr)
{
	/* Set the last 14 bit to the translator  link layer address to avoid
	 * address collissions with the 6LoCAN address range
	 */
	mac_addr[4] = (mac_addr[4] & 0xC0) | (NET_CAN_ETH_TRANSLATOR_ADDR >> 8);
	mac_addr[5] = NET_CAN_ETH_TRANSLATOR_ADDR & 0xFF;
}

static void enable_canbus_eth_translator_filter(ETH_HandleTypeDef *heth, uint8_t *mac_addr)
{
	heth->Instance->MACA1LR =
		(mac_addr[3] << 24U) | (mac_addr[2] << 16U) | (mac_addr[1] << 8U) | mac_addr[0];

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	heth->Instance->MACA1HR =
		ETH_MACAHR_AE | ETH_MACAHR_MBC_HBITS15_8 | ETH_MACAHR_MBC_HBITS7_0;
#else
	/*enable filter 1 and ignore byte 5 and 6 for filtering*/
	heth->Instance->MACA1HR =
		ETH_MACA1HR_AE | ETH_MACA1HR_MBC_HBits15_8 | ETH_MACA1HR_MBC_HBits7_0;
#endif /* CONFIG_SOC_SERIES_STM32H7X */
}
#endif /*CONFIG_NET_L2_CANBUS_ETH_TRANSLATOR*/

static HAL_StatusTypeDef read_eth_phy_register(ETH_HandleTypeDef *heth, uint32_t PHYAddr,
					       uint32_t PHYReg, uint32_t *RegVal)
{
#if defined(CONFIG_SOC_SERIES_STM32H7X)
	return HAL_ETH_ReadPHYRegister(heth, PHYAddr, PHYReg, RegVal);
#else
	ARG_UNUSED(PHYAddr);
	return HAL_ETH_ReadPHYRegister(heth, PHYReg, RegVal);
#endif /* CONFIG_SOC_SERIES_STM32H7X */
}

static inline void disable_mcast_filter(ETH_HandleTypeDef *heth)
{
	__ASSERT_NO_MSG(heth != NULL);

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	ETH_MACFilterConfigTypeDef MACFilterConf;

	HAL_ETH_GetMACFilterConfig(heth, &MACFilterConf);
	MACFilterConf.HashMulticast = DISABLE;
	MACFilterConf.PassAllMulticast = ENABLE;
	MACFilterConf.HachOrPerfectFilter = DISABLE;

	HAL_ETH_SetMACFilterConfig(heth, &MACFilterConf);

	k_sleep(K_MSEC(1));
#else
	uint32_t tmp = heth->Instance->MACFFR;

	/* disable multicast filtering */
	tmp &= ~(ETH_MULTICASTFRAMESFILTER_PERFECTHASHTABLE | ETH_MULTICASTFRAMESFILTER_HASHTABLE |
		 ETH_MULTICASTFRAMESFILTER_PERFECT);

	/* enable receiving all multicast frames */
	tmp |= ETH_MULTICASTFRAMESFILTER_NONE;

	heth->Instance->MACFFR = tmp;

	/* Wait until the write operation will be taken into account:
	 * at least four TX_CLK/RX_CLK clock cycles
	 */
	tmp = heth->Instance->MACFFR;
	k_sleep(K_MSEC(1));
	heth->Instance->MACFFR = tmp;
#endif /* CONFIG_SOC_SERIES_STM32H7X) */
}

static int eth_tx(const struct device *dev, struct net_pkt *pkt)
{
	struct eth_stm32_hal_dev_data *dev_data = DEV_DATA(dev);
	ETH_HandleTypeDef *heth;
	uint8_t *dma_buffer;
	int res;
	size_t total_len;
	__IO ETH_DMADescTypeDef *dma_tx_desc;
	HAL_StatusTypeDef hal_ret = HAL_OK;

	__ASSERT_NO_MSG(pkt != NULL);
	__ASSERT_NO_MSG(pkt->frags != NULL);
	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(dev_data != NULL);

	heth = &dev_data->heth;

	k_mutex_lock(&dev_data->tx_mutex, K_FOREVER);

	total_len = net_pkt_get_len(pkt);
	if (total_len > ETH_TX_BUF_SIZE) {
		LOG_ERR("PKT too big");
		res = -EIO;
		goto error;
	}

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	const uint32_t cur_tx_desc_idx = 0; /* heth->TxDescList.CurTxDesc; */
#endif

	dma_tx_desc = GET_FIRST_DMA_TX_DESC(heth);
	while (IS_ETH_DMATXDESC_OWN(dma_tx_desc) != (uint32_t)RESET) {
		k_yield();
	}

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	dma_buffer = dma_tx_buffer[cur_tx_desc_idx];
#else
	dma_buffer = (uint8_t *)(dma_tx_desc->Buffer1Addr);
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	if (net_pkt_read(pkt, dma_buffer, total_len)) {
		res = -ENOBUFS;
		goto error;
	}

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	ETH_BufferTypeDef tx_buffer_def[ETH_TXBUF_DEF_NB];

	memset(tx_buffer_def, 0, ETH_TXBUF_DEF_NB * sizeof(ETH_BufferTypeDef));

	tx_buffer_def[cur_tx_desc_idx].buffer = dma_buffer;
	tx_buffer_def[cur_tx_desc_idx].len = total_len;
	tx_buffer_def[cur_tx_desc_idx].next = NULL;

	tx_config.Length = total_len;
	tx_config.TxBuffer = tx_buffer_def;

	/* Reset TX complete interrupt semaphore before TX request*/
	k_sem_reset(&dev_data->tx_int_sem);

	/* tx_buffer is allocated on function stack, we need */
	/* to wait for the transfer to complete */
	/* So it is not freed before the interrupt happens */
	hal_ret = HAL_ETH_Transmit_IT(heth, &tx_config);

	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Transmit: failed!");
		res = -EIO;
		goto error;
	}

	/* Wait for end of TX buffer transmission */
	/* If the semaphore timeout breaks, it means */
	/* an error occurred or IT was not fired */
	if (k_sem_take(&dev_data->tx_int_sem, K_MSEC(ETH_DMA_TX_TIMEOUT_MS)) != 0) {
		LOG_ERR("HAL_ETH_TransmitIT tx_int_sem take timeout");
		res = -EIO;

		/* Content of the packet could be the reason for timeout */
		LOG_HEXDUMP_ERR(dma_buffer, total_len, "eth packet timeout");

		/* Check for errors */
		/* Ethernet device was put in error state */
		/* Error state is unrecoverable ? */
		if (HAL_ETH_GetState(heth) == HAL_ETH_STATE_ERROR) {
			LOG_ERR("%s: ETH in error state: errorcode:%x", __func__,
				HAL_ETH_GetError(heth));
			/* TODO recover from error state by restarting eth */
		}

		/* Check for DMA errors */
		if (HAL_ETH_GetDMAError(heth)) {
			LOG_ERR("%s: ETH DMA error: dmaerror:%x", __func__,
				HAL_ETH_GetDMAError(heth));
			/* DMA fatal bus errors are putting in error state*/
			/* TODO recover from this */
		}

		/* Check for MAC errors */
		if (HAL_ETH_GetDMAError(heth)) {
			LOG_ERR("%s: ETH DMA error: macerror:%x", __func__,
				HAL_ETH_GetDMAError(heth));
			/* MAC errors are putting in error state*/
			/* TODO recover from this */
		}

		goto error;
	}

#else
	hal_ret = HAL_ETH_TransmitFrame(heth, total_len);

	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Transmit: failed!");
		res = -EIO;
		goto error;
	}

	/* When Transmit Underflow flag is set, clear it and issue a
	 * Transmit Poll Demand to resume transmission.
	 */
	if ((heth->Instance->DMASR & ETH_DMASR_TUS) != (uint32_t)RESET) {
		/* Clear TUS ETHERNET DMA flag */
		heth->Instance->DMASR = ETH_DMASR_TUS;
		/* Resume DMA transmission*/
		heth->Instance->DMATPDR = 0;
		res = -EIO;
		goto error;
	}
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	res = 0;
error:
	k_mutex_unlock(&dev_data->tx_mutex);

	return res;
}

static struct net_if *get_iface(struct eth_stm32_hal_dev_data *ctx, uint16_t vlan_tag)
{
#if defined(CONFIG_NET_VLAN)
	struct net_if *iface;

	iface = net_eth_get_vlan_iface(ctx->iface, vlan_tag);
	if (!iface) {
		return ctx->iface;
	}

	return iface;
#else
	ARG_UNUSED(vlan_tag);

	return ctx->iface;
#endif
}

static struct net_pkt *eth_rx(const struct device *dev, uint16_t *vlan_tag)
{
	struct eth_stm32_hal_dev_data *dev_data;
	ETH_HandleTypeDef *heth;
#if !defined(CONFIG_SOC_SERIES_STM32H7X)
	__IO ETH_DMADescTypeDef *dma_rx_desc;
#endif /* !CONFIG_SOC_SERIES_STM32H7X */
	struct net_pkt *pkt;
	size_t total_len;
	uint8_t *dma_buffer;
	HAL_StatusTypeDef hal_ret = HAL_OK;

	__ASSERT_NO_MSG(dev != NULL);

	dev_data = DEV_DATA(dev);

	__ASSERT_NO_MSG(dev_data != NULL);

	heth = &dev_data->heth;

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	if (HAL_ETH_IsRxDataAvailable(heth) != true) {
		/* no frame available */
		return NULL;
	}

	ETH_BufferTypeDef rx_buffer_def;
	uint32_t frame_length = 0;

	hal_ret = HAL_ETH_GetRxDataBuffer(heth, &rx_buffer_def);
	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_GetRxDataBuffer: failed with state: %d", hal_ret);
		return NULL;
	}

	hal_ret = HAL_ETH_GetRxDataLength(heth, &frame_length);
	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_GetRxDataLength: failed with state: %d", hal_ret);
		return NULL;
	}

	LOG_INF("currRxDesc: 0x%x, first desc of last packet: 0x%x", heth->RxDescList.CurRxDesc,
		heth->RxDescList.FirstAppDesc);
	ETH_DMADescTypeDef *dma_rx_desc =
		(ETH_DMADescTypeDef *)heth->RxDescList.RxDesc[(heth->RxDescList.FirstAppDesc) % 4];
	LOG_INF("timestamp high: %d, ts low: %d", dma_rx_desc->DESC1, dma_rx_desc->DESC0);

	dma_rx_desc = (ETH_DMADescTypeDef *)
			      heth->RxDescList.RxDesc[(heth->RxDescList.FirstAppDesc + 1) % 4];
	LOG_INF("timestamp high: %d, ts low: %d", dma_rx_desc->DESC1, dma_rx_desc->DESC0);

	total_len = frame_length;
	dma_buffer = rx_buffer_def.buffer;
#else
	hal_ret = HAL_ETH_GetReceivedFrame_IT(heth);
	if (hal_ret != HAL_OK) {
		/* no frame available */
		return NULL;
	}

	total_len = heth->RxFrameInfos.length;
	dma_buffer = (uint8_t *)heth->RxFrameInfos.buffer;
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	pkt = net_pkt_rx_alloc_with_buffer(get_iface(dev_data, *vlan_tag), total_len, AF_UNSPEC, 0,
					   K_MSEC(100));
	if (!pkt) {
		LOG_ERR("Failed to obtain RX buffer");
		goto release_desc;
	}

	if (net_pkt_write(pkt, dma_buffer, total_len)) {
		LOG_ERR("Failed to append RX buffer to context buffer");
		net_pkt_unref(pkt);
		pkt = NULL;
		goto release_desc;
	}

release_desc:
#if defined(CONFIG_SOC_SERIES_STM32H7X)
	hal_ret = HAL_ETH_BuildRxDescriptors(heth);
	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_BuildRxDescriptors: failed: %d", hal_ret);
	}
#else
	/* Release descriptors to DMA */
	/* Point to first descriptor */
	dma_rx_desc = heth->RxFrameInfos.FSRxDesc;
	/* Set Own bit in Rx descriptors: gives the buffers back to DMA */
	for (int i = 0; i < heth->RxFrameInfos.SegCount; i++) {
		dma_rx_desc->Status |= ETH_DMARXDESC_OWN;
		dma_rx_desc = (ETH_DMADescTypeDef *)(dma_rx_desc->Buffer2NextDescAddr);
	}

	/* Clear Segment_Count */
	heth->RxFrameInfos.SegCount = 0;

	/* When Rx Buffer unavailable flag is set: clear it
	 * and resume reception.
	 */
	if ((heth->Instance->DMASR & ETH_DMASR_RBUS) != (uint32_t)RESET) {
		/* Clear RBUS ETHERNET DMA flag */
		heth->Instance->DMASR = ETH_DMASR_RBUS;
		/* Resume DMA reception */
		heth->Instance->DMARPDR = 0;
	}
#endif /* CONFIG_SOC_SERIES_STM32H7X */

#if defined(CONFIG_NET_VLAN)
	struct net_eth_hdr *hdr = NET_ETH_HDR(pkt);

	if (ntohs(hdr->type) == NET_ETH_PTYPE_VLAN) {
		struct net_eth_vlan_hdr *hdr_vlan = (struct net_eth_vlan_hdr *)NET_ETH_HDR(pkt);

		net_pkt_set_vlan_tci(pkt, ntohs(hdr_vlan->vlan.tci));
		*vlan_tag = net_pkt_vlan_tag(pkt);

#if CONFIG_NET_TC_RX_COUNT > 1
		enum net_priority prio;

		prio = net_vlan2priority(net_pkt_vlan_priority(pkt));
		net_pkt_set_priority(pkt, prio);
#endif
	} else {
		net_pkt_set_iface(pkt, dev_data->iface);
	}
#endif /* CONFIG_NET_VLAN */

	if (!pkt) {
		eth_stats_update_errors_rx(get_iface(dev_data, *vlan_tag));
	}

	return pkt;
}

static void rx_thread(void *arg1, void *unused1, void *unused2)
{
	uint16_t vlan_tag = NET_VLAN_TAG_UNSPEC;
	const struct device *dev;
	struct eth_stm32_hal_dev_data *dev_data;
	struct net_pkt *pkt;
	int res;
	uint32_t status;
	HAL_StatusTypeDef hal_ret = HAL_OK;

	__ASSERT_NO_MSG(arg1 != NULL);
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	dev = (const struct device *)arg1;
	dev_data = DEV_DATA(dev);

	__ASSERT_NO_MSG(dev_data != NULL);

	while (1) {
		res = k_sem_take(&dev_data->rx_int_sem,
				 K_MSEC(CONFIG_ETH_STM32_CARRIER_CHECK_RX_IDLE_TIMEOUT_MS));
		if (res == 0) {
			/* semaphore taken, update link status and receive packets */
			if (dev_data->link_up != true) {
				dev_data->link_up = true;
				net_eth_carrier_on(get_iface(dev_data, vlan_tag));
			}
			while ((pkt = eth_rx(dev, &vlan_tag)) != NULL) {
				res = net_recv_data(net_pkt_iface(pkt), pkt);
				if (res < 0) {
					eth_stats_update_errors_rx(net_pkt_iface(pkt));
					LOG_ERR("Failed to enqueue frame "
						"into RX queue: %d",
						res);
					net_pkt_unref(pkt);
				}
			}
		} else if (res == -EAGAIN) {
			/* semaphore timeout period expired, check link status */
			hal_ret = read_eth_phy_register(&dev_data->heth, PHY_ADDR, PHY_BSR,
							(uint32_t *)&status);
			if (hal_ret == HAL_OK) {
				if ((status & PHY_LINKED_STATUS) == PHY_LINKED_STATUS) {
					if (dev_data->link_up != true) {
						dev_data->link_up = true;
						net_eth_carrier_on(get_iface(dev_data, vlan_tag));
					}
				} else {
					if (dev_data->link_up != false) {
						dev_data->link_up = false;
						net_eth_carrier_off(get_iface(dev_data, vlan_tag));
					}
				}
			}
		}
	}
}

static void eth_isr(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data;
	ETH_HandleTypeDef *heth;

	__ASSERT_NO_MSG(dev != NULL);

	dev_data = DEV_DATA(dev);

	__ASSERT_NO_MSG(dev_data != NULL);

	heth = &dev_data->heth;

	__ASSERT_NO_MSG(heth != NULL);

	HAL_ETH_IRQHandler(heth);
}
#ifdef CONFIG_SOC_SERIES_STM32H7X
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth_handle)
{
	__ASSERT_NO_MSG(heth_handle != NULL);

	struct eth_stm32_hal_dev_data *dev_data =
		CONTAINER_OF(heth_handle, struct eth_stm32_hal_dev_data, heth);

	__ASSERT_NO_MSG(dev_data != NULL);

	k_sem_give(&dev_data->tx_int_sem);
}
/* DMA and MAC errors callback only appear in H7 series */
void HAL_ETH_DMAErrorCallback(ETH_HandleTypeDef *heth_handle)
{
	__ASSERT_NO_MSG(heth_handle != NULL);

	LOG_ERR("%s errorcode:%x dmaerror:%x", __func__, HAL_ETH_GetError(heth_handle),
		HAL_ETH_GetDMAError(heth_handle));

	/* State of eth handle is ERROR in case of unrecoverable error */
	/* unrecoverable (ETH_DMACSR_FBE | ETH_DMACSR_TPS | ETH_DMACSR_RPS) */
	if (HAL_ETH_GetState(heth_handle) == HAL_ETH_STATE_ERROR) {
		LOG_ERR("%s ethernet in error state", __func__);
		/* TODO restart the ETH peripheral to recover */
		return;
	}

	/* Recoverable errors don't put ETH in error state */
	/* ETH_DMACSR_CDE | ETH_DMACSR_ETI | ETH_DMACSR_RWT */
	/* | ETH_DMACSR_RBU | ETH_DMACSR_AIS) */

	/* TODO Check if we were TX transmitting and the unlock semaphore */
	/* To return the error as soon as possible else we'll just wait */
	/* for the timeout */
}
void HAL_ETH_MACErrorCallback(ETH_HandleTypeDef *heth_handle)
{
	__ASSERT_NO_MSG(heth_handle != NULL);

	/* MAC errors dumping */
	LOG_ERR("%s errorcode:%x macerror:%x", __func__, HAL_ETH_GetError(heth_handle),
		HAL_ETH_GetMACError(heth_handle));

	/* State of eth handle is ERROR in case of unrecoverable error */
	if (HAL_ETH_GetState(heth_handle) == HAL_ETH_STATE_ERROR) {
		LOG_ERR("%s ethernet in error state", __func__);
		/* TODO restart or reconfig ETH peripheral to recover */

		return;
	}
}
#endif /* CONFIG_SOC_SERIES_STM32H7X */

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth_handle)
{
	__ASSERT_NO_MSG(heth_handle != NULL);

	struct eth_stm32_hal_dev_data *dev_data =
		CONTAINER_OF(heth_handle, struct eth_stm32_hal_dev_data, heth);

	__ASSERT_NO_MSG(dev_data != NULL);

	k_sem_give(&dev_data->rx_int_sem);
}

#if defined(CONFIG_ETH_STM32_HAL_RANDOM_MAC)
static void generate_mac(uint8_t *mac_addr)
{
	gen_random_mac(mac_addr, ST_OUI_B0, ST_OUI_B1, ST_OUI_B2);
}
#endif

static int eth_initialize(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data;
	const struct eth_stm32_hal_dev_cfg *cfg;
	ETH_HandleTypeDef *heth;
	HAL_StatusTypeDef hal_ret = HAL_OK;
	int ret = 0;

	__ASSERT_NO_MSG(dev != NULL);

	dev_data = DEV_DATA(dev);
	cfg = DEV_CFG(dev);

	__ASSERT_NO_MSG(dev_data != NULL);
	__ASSERT_NO_MSG(cfg != NULL);

	dev_data->clock = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);

	/* enable clock */
	ret = clock_control_on(dev_data->clock, (clock_control_subsys_t *)&cfg->pclken);
	ret |= clock_control_on(dev_data->clock, (clock_control_subsys_t *)&cfg->pclken_tx);
	ret |= clock_control_on(dev_data->clock, (clock_control_subsys_t *)&cfg->pclken_rx);
#if !defined(CONFIG_SOC_SERIES_STM32H7X)
	ret |= clock_control_on(dev_data->clock, (clock_control_subsys_t *)&cfg->pclken_ptp);
#endif /* !defined(CONFIG_SOC_SERIES_STM32H7X) */

	if (ret) {
		LOG_ERR("Failed to enable ethernet clock");
		return -EIO;
	}

	/* configure pinmux */
	ret = stm32_dt_pinctrl_configure(cfg->pinctrl, cfg->pinctrl_len,
					 (uint32_t)dev_data->heth.Instance);
	if (ret < 0) {
		LOG_ERR("Could not configure ethernet pins");
		return ret;
	}

	heth = &dev_data->heth;

#if defined(CONFIG_ETH_STM32_HAL_RANDOM_MAC)
	generate_mac(dev_data->mac_addr);
#endif
#if defined(CONFIG_NET_L2_CANBUS_ETH_TRANSLATOR)
	set_mac_to_translator_addr(dev_data->mac_addr);
#endif

	heth->Init.MACAddr = dev_data->mac_addr;

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	heth->Init.TxDesc = dma_tx_desc_tab;
	heth->Init.RxDesc = dma_rx_desc_tab;
	heth->Init.RxBuffLen = ETH_RX_BUF_SIZE;
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	hal_ret = HAL_ETH_Init(heth);
	if (hal_ret == HAL_TIMEOUT) {
		/* HAL Init time out. This could be linked to */
		/* a recoverable error. Log the issue and continue */
		/* driver initialisation */
		LOG_ERR("HAL_ETH_Init Timed out");
	} else if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Init failed: %d", hal_ret);
		return -EINVAL;
	}

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	/* Tx config init: */
	memset(&tx_config, 0, sizeof(ETH_TxPacketConfig));
	tx_config.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
	tx_config.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
	tx_config.CRCPadCtrl = ETH_CRC_PAD_INSERT;
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	dev_data->link_up = false;

	/* Initialize semaphores */
	k_mutex_init(&dev_data->tx_mutex);
	k_sem_init(&dev_data->rx_int_sem, 0, K_SEM_MAX_LIMIT);
#ifdef CONFIG_SOC_SERIES_STM32H7X
	k_sem_init(&dev_data->tx_int_sem, 0, K_SEM_MAX_LIMIT);
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	/* Start interruption-poll thread */
	k_thread_create(&dev_data->rx_thread, dev_data->rx_thread_stack,
			K_KERNEL_STACK_SIZEOF(dev_data->rx_thread_stack), rx_thread, (void *)dev,
			NULL, NULL, K_PRIO_COOP(CONFIG_ETH_STM32_HAL_RX_THREAD_PRIO), 0, K_NO_WAIT);

	k_thread_name_set(&dev_data->rx_thread, "stm_eth");

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	for (uint32_t i = 0; i < ETH_RX_DESC_CNT; i++) {
		hal_ret = HAL_ETH_DescAssignMemory(heth, i, dma_rx_buffer[i], NULL);
		if (hal_ret != HAL_OK) {
			LOG_ERR("HAL_ETH_DescAssignMemory: failed: %d, i: %d", hal_ret, i);
			return -EINVAL;
		}
	}

	enablePTP(heth);

	hal_ret = HAL_ETH_Start_IT(heth);
#else
	HAL_ETH_DMATxDescListInit(heth, dma_tx_desc_tab, &dma_tx_buffer[0][0], ETH_TXBUFNB);
	HAL_ETH_DMARxDescListInit(heth, dma_rx_desc_tab, &dma_rx_buffer[0][0], ETH_RXBUFNB);

	hal_ret = HAL_ETH_Start(heth);
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Start{_IT} failed");
	}

	disable_mcast_filter(heth);

#if defined(CONFIG_NET_L2_CANBUS_ETH_TRANSLATOR)
	enable_canbus_eth_translator_filter(heth, dev_data->mac_addr);
#endif

#if defined(CONFIG_SOC_SERIES_STM32H7X)
	/* Adjust MDC clock range depending on HCLK frequency: */
	HAL_ETH_SetMDIOClockRange(heth);

	/* @TODO: read duplex mode and speed from PHY and set it to ETH */

	ETH_MACConfigTypeDef mac_config;

	HAL_ETH_GetMACConfig(heth, &mac_config);
	mac_config.DuplexMode = ETH_FULLDUPLEX_MODE;
	mac_config.Speed = ETH_SPEED_100M;
	HAL_ETH_SetMACConfig(heth, &mac_config);
#endif /* CONFIG_SOC_SERIES_STM32H7X */

	LOG_DBG("MAC %02x:%02x:%02x:%02x:%02x:%02x", dev_data->mac_addr[0], dev_data->mac_addr[1],
		dev_data->mac_addr[2], dev_data->mac_addr[3], dev_data->mac_addr[4],
		dev_data->mac_addr[5]);

	return 0;
}

static void eth_iface_init(struct net_if *iface)
{
	const struct device *dev;
	struct eth_stm32_hal_dev_data *dev_data;
	bool is_first_init = false;

	__ASSERT_NO_MSG(iface != NULL);

	dev = net_if_get_device(iface);
	__ASSERT_NO_MSG(dev != NULL);

	dev_data = DEV_DATA(dev);
	__ASSERT_NO_MSG(dev_data != NULL);

	/* For VLAN, this value is only used to get the correct L2 driver.
	 * The iface pointer in context should contain the main interface
	 * if the VLANs are enabled.
	 */
	if (dev_data->iface == NULL) {
		dev_data->iface = iface;
		is_first_init = true;
	}

	/* Register Ethernet MAC Address with the upper layer */
	net_if_set_link_addr(iface, dev_data->mac_addr, sizeof(dev_data->mac_addr),
			     NET_LINK_ETHERNET);

	ethernet_init(iface);

	net_if_flag_set(iface, NET_IF_NO_AUTO_START);

	if (is_first_init) {
		/* Now that the iface is setup, we are safe to enable IRQs. */
		__ASSERT_NO_MSG(DEV_CFG(dev)->config_func != NULL);
		DEV_CFG(dev)->config_func();
	}
}

static enum ethernet_hw_caps eth_stm32_hal_get_capabilities(const struct device *dev)
{
	ARG_UNUSED(dev);

	return ETHERNET_LINK_10BASE_T | ETHERNET_LINK_100BASE_T
#if defined(CONFIG_NET_VLAN)
	       | ETHERNET_HW_VLAN
#endif
		;
}

static int eth_stm32_hal_set_config(const struct device *dev, enum ethernet_config_type type,
				    const struct ethernet_config *config)
{
	struct eth_stm32_hal_dev_data *dev_data;
	ETH_HandleTypeDef *heth;

	switch (type) {
	case ETHERNET_CONFIG_TYPE_MAC_ADDRESS:
		dev_data = DEV_DATA(dev);
		heth = &dev_data->heth;

		memcpy(dev_data->mac_addr, config->mac_address.addr, 6);
		heth->Instance->MACA0HR = (dev_data->mac_addr[5] << 8) | dev_data->mac_addr[4];
		heth->Instance->MACA0LR = (dev_data->mac_addr[3] << 24) |
					  (dev_data->mac_addr[2] << 16) |
					  (dev_data->mac_addr[1] << 8) | dev_data->mac_addr[0];
		net_if_set_link_addr(dev_data->iface, dev_data->mac_addr,
				     sizeof(dev_data->mac_addr), NET_LINK_ETHERNET);
		return 0;
	default:
		break;
	}

	return -ENOTSUP;
}

static const struct ethernet_api eth_api = {
	.iface_api.init = eth_iface_init,

	.get_capabilities = eth_stm32_hal_get_capabilities,
	.set_config = eth_stm32_hal_set_config,
	.send = eth_tx,
};

static void eth0_irq_config(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), eth_isr, DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

static const struct soc_gpio_pinctrl eth0_pins[] = ST_STM32_DT_INST_PINCTRL(0, 0);

static const struct eth_stm32_hal_dev_cfg eth0_config = {
	.config_func = eth0_irq_config,
	.pclken = { .bus = DT_INST_CLOCKS_CELL_BY_NAME(0, stmmaceth, bus),
		    .enr = DT_INST_CLOCKS_CELL_BY_NAME(0, stmmaceth, bits) },
	.pclken_tx = { .bus = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_tx, bus),
		       .enr = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_tx, bits) },
	.pclken_rx = { .bus = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_rx, bus),
		       .enr = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_rx, bits) },
#if !defined(CONFIG_SOC_SERIES_STM32H7X)
	.pclken_ptp = { .bus = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_ptp, bus),
			.enr = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_ptp, bits) },
#endif /* !CONFIG_SOC_SERIES_STM32H7X */
	.pinctrl = eth0_pins,
	.pinctrl_len = ARRAY_SIZE(eth0_pins),
};

static struct eth_stm32_hal_dev_data eth0_data = {
	.heth = {
		.Instance = (ETH_TypeDef *)DT_INST_REG_ADDR(0),
		.Init = {
#if !defined(CONFIG_SOC_SERIES_STM32H7X)
#if defined(CONFIG_ETH_STM32_AUTO_NEGOTIATION_ENABLE)
			.AutoNegotiation = ETH_AUTONEGOTIATION_ENABLE,
#else
			.AutoNegotiation = ETH_AUTONEGOTIATION_DISABLE,
#if defined(CONFIG_ETH_STM32_SPEED_10M)
			.Speed = ETH_SPEED_10M,
#else
			.Speed = ETH_SPEED_100M,
#endif
#if defined(CONFIG_ETH_STM32_MODE_HALFDUPLEX)
			.DuplexMode = ETH_MODE_HALFDUPLEX,
#else
			.DuplexMode = ETH_MODE_FULLDUPLEX,
#endif
#endif /* !CONFIG_ETH_STM32_AUTO_NEGOTIATION_ENABLE */
			.PhyAddress = PHY_ADDR,
			.RxMode = ETH_RXINTERRUPT_MODE,
			.ChecksumMode = ETH_CHECKSUM_BY_SOFTWARE,
#endif /* !CONFIG_SOC_SERIES_STM32H7X */
#if defined(CONFIG_ETH_STM32_HAL_MII)
			.MediaInterface = ETH_MEDIA_INTERFACE_MII,
#else
			.MediaInterface = ETH_MEDIA_INTERFACE_RMII,
#endif
		},
	},
	.mac_addr = {
		ST_OUI_B0,
		ST_OUI_B1,
		ST_OUI_B2,
#if !defined(CONFIG_ETH_STM32_HAL_RANDOM_MAC)
		CONFIG_ETH_STM32_HAL_MAC3,
		CONFIG_ETH_STM32_HAL_MAC4,
		CONFIG_ETH_STM32_HAL_MAC5
#endif
	},
};

ETH_NET_DEVICE_DT_INST_DEFINE(0, eth_initialize, NULL, &eth0_data, &eth0_config,
			      CONFIG_ETH_INIT_PRIORITY, &eth_api, ETH_STM32_HAL_MTU);
<<<<<<< HEAD
=======

#if defined(CONFIG_PTP_CLOCK_STM32) && defined(CONFIG_SOC_SERIES_STM32H7X)
struct ptp_context {
	struct eth_stm32_hal_dev_data *eth_dev_data;
}

static struct ptp_context ptp_stm32_0_context;

void HAL_PTPSetTime(ETH_HandleTypeDef *heth, stm32_ptp_time_t *time)
{
	heth->Instance->MACSTSUR = time->second;
	heth->Instance->MACSTNUR = time->nanosecond;

	heth->Instance->MACTSCR |= ETH_MACTSCR_TSINIT;
	return;
}

static int ptp_clock_stm32_set(const struct device *dev, struct net_ptp_time *tm)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	stm32_ptp_time_t stm32_time;

	stm32_time.second = tm->_sec.low;
	stm32_time.nanosecond = tm->nanosecond;

	HAL_PTPSetTime(&(eth_dev_data->heth), &stm32_time);
	return 0;
}

void HAL_PTPGetTime(ETH_HandleTypeDef *heth, stm32_ptp_time_t *time)
{
	time->second = heth->Instance->MACSTSR;
	time->nanosecond = heth->Instance->MACSTNR;
	return;
}

static int ptp_clock_stm32_get(const struct device *dev, struct net_ptp_time *tm)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	stm32_ptp_time_t stm32_time;

	HAL_PTPGetTime(&(eth_dev_data->heth), &stm32_time);

	tm->second = (uint64_t)stm32_time.second;
	tm->nanosecond = stm32_time.nanosecond;
	return 0;
}

void HAL_PTPAdjustNsTime(ETH_HandleTypeDef *heth, int32_t ns_increment)
{
	heth->Instance->MACSTSUR = 0x00; // seconds update register
	heth->Instance->MACSTNUR = ns_increment; // nanoseconds update register
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSUPDT;

	while ((heth->Instance->MACTSCR & ETH_MACTSCR_TSUPDT_Msk) >> ETH_MACTSCR_TSUPDT_Pos != 0)
		;
}

/**
 * Adjust by increment nanoseconds.
 */
static int ptp_clock_stm32_adjust(const struct device *dev, int increment)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	int ret = 0;

	if ((increment <= -NSEC_PER_SEC) || (increment >= NSEC_PER_SEC)) {
		ret = -EINVAL;
	} else {
		HAL_PTPAdjustNsTime(&(eth_dev_data->heth), increment);
	}

	return ret;
}

uint32_t HAL_PTPGetAddendRegister(ETH_HandleTypeDef *heth)
{
	return heth->Instance->MACTSAR;
}

void HAL_PTPSetAddendRegister(ETH_HandleTypeDef *heth, uint32_t new_val)
{
	heth->Instance->MACTSAR = new_val;
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSADDREG;
	while ((heth->Instance->MACTSCR & ETH_MACTSCR_TSADDREG) >> ETH_MACTSCR_TSADDREG_Pos != 0)
		;
}

/*
 * Assuming that ratio is the ratio by which we should multiply the current rate, not the original rate.
 */
static int ptp_clock_stm32_rate_adjust(const struct device *dev, float ratio)
{
	const uint32_t hw_inc =
		NSEC_PER_SEC /
		CONFIG_ETH_STM32_PTP_CLOCK_SRC_HZ; // this is 10ns for us, 10^9 / 100 * 10^6
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;

	uint32_t new_val, val = HAL_PTPGetAddendRegister(&(eth_dev_data->heth));

	/* No change needed. */
	if (ratio == 1.0) {
		return 0;
	}

	new_val = (uint32_t)val * ratio;

	if (new_val >= INT32_MAX) {
		/* Value is too high.
		 * It is not possible to adjust the rate of the clock.
		 */
		new_val = 0xFFFFFFFF;
		ratio = ((float)new_val) / val;
	}

	/* Save new ratio. */
	eth_dev_data->clk_ratio *= ratio;

	HAL_PTPSetAddendRegister(&(eth_dev_data->heth), new_val);

	return 0;
}

static const struct ptp_clock_driver_api api = {
	.set = ptp_clock_stm32_set,
	.get = ptp_clock_stm32_get,
	.adjust = ptp_clock_stm32_adjust,
	.rate_adjust = ptp_clock_stm32_rate_adjust,
};

static int ptp_stm32_init(const struct device *dev)
{
	const struct device *eth_dev = DEVICE_DT_GET(DT_NODELABEL(enet));
	struct eth_context *context = eth_dev->data;
	struct ptp_context *ptp_context = port->data;

	context->ptp_clock = port;
	ptp_context->eth_context = context;

	return 0;
}

DEVICE_DEFINE(stm32_ptp_clock_0, PTP_CLOCK_NAME, ptp_stm32_init, NULL, &ptp_stm32_0_context, NULL,
	      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, &api);

#endif /* CONFIG_PTP_CLOCK_STM32 */
>>>>>>> b30f84c926 (create ptp api functions in stm32 eth driver only for h7xx boards)
