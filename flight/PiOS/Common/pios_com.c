/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_COM COM layer functions
 * @brief Hardware communication layer
 * @{
 *
 * @file       pios_com.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2012-2014
 * @author     dRonin, http://dRonin.org/, Copyright (C) 2016
 * @brief      COM layer functions
 * @see        The GNU Public License (GPL) Version 3
 *
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
 *
 * Additional note on redistribution: The copyright and license notices above
 * must be maintained in each individual source file that is a derivative work
 * of this source file; otherwise redistribution is prohibited.
 */

/* Project Includes */
#include "pios.h"

#if defined(PIOS_INCLUDE_COM)

#include "fifo_buffer.h"
#include <pios_com_priv.h>

#if !defined(PIOS_INCLUDE_FREERTOS) && !defined(PIOS_INCLUDE_CHIBIOS)
#include "pios_delay.h"		/* PIOS_DELAY_WaitmS */
#endif

#include "pios_semaphore.h"
#include "pios_mutex.h"

enum pios_com_dev_magic {
  PIOS_COM_DEV_MAGIC = 0xaa55aa55,
};

struct pios_com_dev {
	enum pios_com_dev_magic magic;
	uintptr_t lower_id;
	const struct pios_com_driver * driver;

#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
	struct pios_semaphore *tx_sem;
	struct pios_semaphore *rx_sem;
	struct pios_mutex *sendbuffer_mtx;
#endif

	bool has_rx;
	bool has_tx;

	t_fifo_buffer rx;
	t_fifo_buffer tx;
};

static bool PIOS_COM_validate(struct pios_com_dev *com_dev)
{
	return (com_dev && (com_dev->magic == PIOS_COM_DEV_MAGIC));
}

static struct pios_com_dev *PIOS_COM_alloc(void)
{
	struct pios_com_dev *com_dev;

	com_dev = (struct pios_com_dev *)PIOS_malloc(sizeof(*com_dev));
	if (!com_dev) return (NULL);

	memset(com_dev, 0, sizeof(*com_dev));
	com_dev->magic = PIOS_COM_DEV_MAGIC;
	return(com_dev);
}

static uint16_t PIOS_COM_TxOutCallback(uintptr_t context, uint8_t * buf, uint16_t buf_len, uint16_t * headroom, bool * need_yield);
static uint16_t PIOS_COM_RxInCallback(uintptr_t context, uint8_t * buf, uint16_t buf_len, uint16_t * headroom, bool * need_yield);
static void PIOS_COM_UnblockRx(struct pios_com_dev *com_dev, bool * need_yield);
static void PIOS_COM_UnblockTx(struct pios_com_dev *com_dev, bool * need_yield);

/**
  * Initialises COM layer
  * \param[out] handle
  * \param[in] driver
  * \param[in] id
  * \return < 0 if initialisation failed
  */
int32_t PIOS_COM_Init(uintptr_t * com_id, const struct pios_com_driver * driver, uintptr_t lower_id, uint16_t rx_buffer_len, uint16_t tx_buffer_len)
{
	PIOS_Assert(com_id);
	PIOS_Assert(driver);

	uint8_t *rx_buffer, *tx_buffer;


	bool has_rx = false;
	bool has_tx = false;

	PIOS_Assert(has_rx || has_tx);
	PIOS_Assert(driver->bind_tx_cb || !has_tx);
	PIOS_Assert(driver->bind_rx_cb || !has_rx);

	struct pios_com_dev *com_dev;

	com_dev = (struct pios_com_dev *) PIOS_COM_alloc();
	if (!com_dev) goto out_fail;

	if (rx_buffer_len) {
		has_rx = true;
		rx_buffer = PIOS_malloc(rx_buffer_len);
		if (!rx_buffer) goto out_fail;
	}


	if (tx_buffer_len) {
		has_tx = true;
		tx_buffer = PIOS_malloc(tx_buffer_len);
		if (!tx_buffer) goto out_fail;
	}

	com_dev->driver   = driver;
	com_dev->lower_id = lower_id;

	com_dev->has_rx = has_rx;
	com_dev->has_tx = has_tx;

	if (has_rx) {
		fifoBuf_init(&com_dev->rx, rx_buffer, rx_buffer_len);
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
		com_dev->rx_sem = PIOS_Semaphore_Create();
#endif	/* PIOS_INCLUDE_FREERTOS */
		(com_dev->driver->bind_rx_cb)(lower_id, PIOS_COM_RxInCallback, (uintptr_t)com_dev);
		if (com_dev->driver->rx_start) {
			/* Start the receiver */
			(com_dev->driver->rx_start)(com_dev->lower_id,
						    fifoBuf_getFree(&com_dev->rx));
		}
	}

	if (has_tx) {
		fifoBuf_init(&com_dev->tx, tx_buffer, tx_buffer_len);
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
		com_dev->tx_sem = PIOS_Semaphore_Create();
#endif	/* PIOS_INCLUDE_FREERTOS */
		(com_dev->driver->bind_tx_cb)(lower_id, PIOS_COM_TxOutCallback, (uintptr_t)com_dev);
	}
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
	com_dev->sendbuffer_mtx = PIOS_Mutex_Create();
#endif /* PIOS_INCLUDE_FREERTOS */

	*com_id = (uintptr_t)com_dev;
	return(0);

out_fail:
	return(-1);
}

static void PIOS_COM_UnblockRx(struct pios_com_dev *com_dev, bool * need_yield)
{
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
	if (PIOS_IRQ_InISR() == true)
		PIOS_Semaphore_Give_FromISR(com_dev->rx_sem, need_yield);
	else
		PIOS_Semaphore_Give(com_dev->rx_sem);
#endif
}

static void PIOS_COM_UnblockTx(struct pios_com_dev *com_dev, bool * need_yield)
{
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
	if (PIOS_IRQ_InISR() == true)
		PIOS_Semaphore_Give_FromISR(com_dev->tx_sem, need_yield);
	else
		PIOS_Semaphore_Give(com_dev->tx_sem);
#endif
}

static uint16_t PIOS_COM_RxInCallback(uintptr_t context, uint8_t * buf, uint16_t buf_len, uint16_t * headroom, bool * need_yield)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)context;

	bool valid = PIOS_COM_validate(com_dev);
	PIOS_Assert(valid);
	PIOS_Assert(com_dev->has_rx);

	uint16_t bytes_into_fifo = fifoBuf_putData(&com_dev->rx, buf, buf_len);

	if (bytes_into_fifo > 0) {
		/* Data has been added to the buffer */
		PIOS_COM_UnblockRx(com_dev, need_yield);
	}

	if (headroom) {
		*headroom = fifoBuf_getFree(&com_dev->rx);
	}

	return (bytes_into_fifo);
}

static uint16_t PIOS_COM_TxOutCallback(uintptr_t context, uint8_t * buf, uint16_t buf_len, uint16_t * headroom, bool * need_yield)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)context;

	bool valid = PIOS_COM_validate(com_dev);
	PIOS_Assert(valid);
	PIOS_Assert(buf);
	PIOS_Assert(buf_len);
	PIOS_Assert(com_dev->has_tx);

	uint16_t bytes_from_fifo = fifoBuf_getData(&com_dev->tx, buf, buf_len);

	if (bytes_from_fifo > 0) {
		/* More space has been made in the buffer */
		PIOS_COM_UnblockTx(com_dev, need_yield);
	}

	if (headroom) {
		*headroom = fifoBuf_getUsed(&com_dev->tx);
	}

	return (bytes_from_fifo);
}

/**
* Change the port speed without re-initializing
* \param[in] port COM port
* \param[in] baud Requested baud rate
* \return -1 if port not available
* \return 0 on success
*/
int32_t PIOS_COM_ChangeBaud(uintptr_t com_id, uint32_t baud)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		/* Undefined COM port for this board (see pios_board.c) */
		return -1;
	}

	/* Invoke the driver function if it exists */
	if (com_dev->driver->set_baud) {
		com_dev->driver->set_baud(com_dev->lower_id, baud);
	}

	return 0;
}

/**
* Sends a package over given port
* \param[in] port COM port
* \param[in] buffer character buffer
* \param[in] len buffer length
* \return -1 if port not available
* \return -2 if non-blocking mode activated: buffer is full
*            caller should retry until buffer is free again
* \return -3 another thread is already sending, caller should
*            retry until com is available again
* \return number of bytes transmitted on success
*/
int32_t PIOS_COM_SendBufferNonBlocking(uintptr_t com_id, const uint8_t *buffer, uint16_t len)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		/* Undefined COM port for this board (see pios_board.c) */
		return -1;
	}

	PIOS_Assert(com_dev->has_tx);

#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
	if (PIOS_Mutex_Lock(com_dev->sendbuffer_mtx, 0) != true) {
		return -3;
	}
#endif /* defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS) */
	if (com_dev->driver->available && !com_dev->driver->available(com_dev->lower_id)) {
		/*
		 * Underlying device is down/unconnected.
		 * Dump our fifo contents and act like an infinite data sink.
		 * Failure to do this results in stale data in the fifo as well as
		 * possibly having the caller block trying to send to a device that's
		 * no longer accepting data.
		 */
		fifoBuf_clearData(&com_dev->tx);
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
		PIOS_Mutex_Unlock(com_dev->sendbuffer_mtx);
#endif /* PIOS_INCLUDE_FREERTOS */

		return len;
	}

	if (len > fifoBuf_getFree(&com_dev->tx)) {
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
		PIOS_Mutex_Unlock(com_dev->sendbuffer_mtx);
#endif /* PIOS_INCLUDE_FREERTOS */
		/* Buffer cannot accept all requested bytes (retry) */
		return -2;
	}

	uint16_t bytes_into_fifo = fifoBuf_putData(&com_dev->tx, buffer, len);

	if (bytes_into_fifo > 0) {
		/* More data has been put in the tx buffer, make sure the tx is started */
		if (com_dev->driver->tx_start) {
			com_dev->driver->tx_start(com_dev->lower_id,
						  fifoBuf_getUsed(&com_dev->tx));
		}
	}

#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
	PIOS_Mutex_Unlock(com_dev->sendbuffer_mtx);
#endif /* PIOS_INCLUDE_FREERTOS */
	return (bytes_into_fifo);
}

/**
* Sends a package over given port
* (blocking function)
* \param[in] port COM port
* \param[in] buffer character buffer
* \param[in] len buffer length
* \return -1 if port not available
* \return number of bytes transmitted on success
*/
int32_t PIOS_COM_SendBuffer(uintptr_t com_id, const uint8_t *buffer, uint16_t len)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		/* Undefined COM port for this board (see pios_board.c) */
		return -1;
	}

	PIOS_Assert(com_dev->has_tx);

	uint32_t max_frag_len = fifoBuf_getSize(&com_dev->tx);
	uint32_t bytes_to_send = len;
	while (bytes_to_send) {
		uint32_t frag_size;

		if (bytes_to_send > max_frag_len) {
			frag_size = max_frag_len;
		} else {
			frag_size = bytes_to_send;
		}
		int32_t rc = PIOS_COM_SendBufferNonBlocking(com_id, buffer, frag_size);
		if (rc >= 0) {
			bytes_to_send -= rc;
			buffer += rc;
		} else {
			switch (rc) {
			case -1:
				/* Device is invalid, this will never work */
				return -1;
			case -2:
				/* Device is busy, wait for the underlying device to free some space and retry */
				/* Make sure the transmitter is running while we wait */
				if (com_dev->driver->tx_start) {
					(com_dev->driver->tx_start)(com_dev->lower_id,
								fifoBuf_getUsed(&com_dev->tx));
				}
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
				if (PIOS_Semaphore_Take(com_dev->tx_sem, 5000) != true) {
					return -3;
				}
#endif
				continue;
			default:
				/* Unhandled return code */
				return rc;
			}
		}
	}

	return len;
}

/**
* Sends a single character over given port
* \param[in] port COM port
* \param[in] c character
* \return -1 if port not available
* \return -2 buffer is full
*            caller should retry until buffer is free again
* \return 0 on success
*/
int32_t PIOS_COM_SendCharNonBlocking(uintptr_t com_id, char c)
{
	return PIOS_COM_SendBufferNonBlocking(com_id, (uint8_t *)&c, 1);
}

/**
* Sends a single character over given port
* (blocking function)
* \param[in] port COM port
* \param[in] c character
* \return -1 if port not available
* \return 0 on success
*/
int32_t PIOS_COM_SendChar(uintptr_t com_id, char c)
{
	return PIOS_COM_SendBuffer(com_id, (uint8_t *)&c, 1);
}

/**
* Sends a string over given port
* \param[in] port COM port
* \param[in] str zero-terminated string
* \return -1 if port not available
* \return -2 buffer is full
*         caller should retry until buffer is free again
* \return 0 on success
*/
int32_t PIOS_COM_SendStringNonBlocking(uintptr_t com_id, const char *str)
{
	return PIOS_COM_SendBufferNonBlocking(com_id, (uint8_t *)str, (uint16_t)strlen(str));
}

/**
* Sends a string over given port
* (blocking function)
* \param[in] port COM port
* \param[in] str zero-terminated string
* \return -1 if port not available
* \return 0 on success
*/
int32_t PIOS_COM_SendString(uintptr_t com_id, const char *str)
{
	return PIOS_COM_SendBuffer(com_id, (uint8_t *)str, strlen(str));
}

/**
* Sends a formatted string (-> printf) over given port
* \param[in] port COM port
* \param[in] *format zero-terminated format string - 128 characters supported maximum!
* \param[in] ... optional arguments,
*        128 characters supported maximum!
* \return -2 if non-blocking mode activated: buffer is full
*         caller should retry until buffer is free again
* \return 0 on success
*/
int32_t PIOS_COM_SendFormattedStringNonBlocking(uintptr_t com_id, const char *format, ...)
{
	uint8_t buffer[128]; // TODO: tmp!!! Provide a streamed COM method later!

	va_list args;

	va_start(args, format);
	vsprintf((char *)buffer, format, args);
	return PIOS_COM_SendBufferNonBlocking(com_id, buffer, (uint16_t)strlen((char *)buffer));
}

/**
* Sends a formatted string (-> printf) over given port
* (blocking function)
* \param[in] port COM port
* \param[in] *format zero-terminated format string - 128 characters supported maximum!
* \param[in] ... optional arguments,
* \return -1 if port not available
* \return 0 on success
*/
int32_t PIOS_COM_SendFormattedString(uintptr_t com_id, const char *format, ...)
{
	uint8_t buffer[128]; // TODO: tmp!!! Provide a streamed COM method later!
	va_list args;

	va_start(args, format);
	vsprintf((char *)buffer, format, args);
	return PIOS_COM_SendBuffer(com_id, buffer, (uint16_t)strlen((char *)buffer));
}

/**
 * Reports number of bytes available for receiving.
 * \param[in] com_id the COM instance to receive from
 * \returns number of bytes available to be read
 */
uint16_t PIOS_COM_GetNumReceiveBytesPending(uintptr_t com_id) {
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		/* Undefined COM port for this board (see pios_board.c) */
		PIOS_Assert(0);
	}

	PIOS_Assert(com_dev->has_rx);

	uint16_t bytes_from_fifo = fifoBuf_getUsed(&com_dev->rx);

	if (bytes_from_fifo == 0) {
		/* No more bytes in receive buffer */
		/* Make sure the receiver is running */
		if (com_dev->driver->rx_start) {
			/* Notify the lower layer that there is now room in the rx buffer */
			(com_dev->driver->rx_start)(com_dev->lower_id,
						    fifoBuf_getFree(&com_dev->rx));
		}
	}

	return fifoBuf_getUsed(&com_dev->rx);
}

/**
* Transfer bytes from port buffers into another buffer
* \param[in] port COM port
* \returns Byte from buffer
*/
uint16_t PIOS_COM_ReceiveBuffer(uintptr_t com_id, uint8_t * buf, uint16_t buf_len, uint32_t timeout_ms)
{
	PIOS_Assert(buf);
	PIOS_Assert(buf_len);
	uint16_t bytes_from_fifo;

	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		/* Undefined COM port for this board (see pios_board.c) */
		PIOS_Assert(0);
	}
	PIOS_Assert(com_dev->has_rx);

	/* Clear any pending RX wakeup */
	PIOS_Semaphore_Take(com_dev->rx_sem, 0);

check_again:
	bytes_from_fifo = fifoBuf_getData(&com_dev->rx, buf, buf_len);

	if (bytes_from_fifo == 0) {
		/* No more bytes in receive buffer */
		/* Make sure the receiver is running while we wait */
		if (com_dev->driver->rx_start) {
			/* Notify the lower layer that there is now room in the rx buffer */
			(com_dev->driver->rx_start)(com_dev->lower_id,
						    fifoBuf_getFree(&com_dev->rx));
		}
		if (timeout_ms > 0) {
#if defined(PIOS_INCLUDE_FREERTOS) || defined(PIOS_INCLUDE_CHIBIOS)
			if (PIOS_Semaphore_Take(com_dev->rx_sem, timeout_ms) == true) {
				/* Make sure we don't come back here again */
				timeout_ms = 0;
				goto check_again;
			}
#else
			PIOS_DELAY_WaitmS(1);
			timeout_ms--;
			goto check_again;
#endif
		}
	}

	/* Return received byte */
	return (bytes_from_fifo);
}

/**
 * Query if a com port is available for use.  That can be
 * used to check a link is established even if the device
 * is valid.
 */
bool PIOS_COM_Available(uintptr_t com_id)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		return false;
	}

	// If a driver does not provide a query method assume always
	// available if valid
	if (com_dev->driver->available == NULL)
		return true;

	return (com_dev->driver->available)(com_dev->lower_id);
}

uintptr_t PIOS_COM_GetDriverCtx(uintptr_t com_id) {
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
		return false;
	}

	return com_dev->lower_id;
}

#endif

/**
 * @}
 * @}
 */
