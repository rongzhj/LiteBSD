/*
 * MRF24WG RAW (Random Access Window)
 *
 * Functions to control RAW windows.
 */
#include "wf_universal_driver.h"
#include "wf_global_includes.h"

#define WF_RAW_STATUS_REG_BUSY_MASK     ((u_int16_t)(0x0001))

/*
 * These macros set a flag bit if the raw index is set past the end
 * of the raw window, or clear the flag bit if the raw index is set
 * within the raw window.
 */
#define SetIndexOutOfBoundsFlag(rawId)      g_RawIndexPastEnd |= g_RawAccessOutOfBoundsMask[rawId]
#define ClearIndexOutOfBoundsFlag(rawId)    g_RawIndexPastEnd &= ~g_RawAccessOutOfBoundsMask[rawId]
#define isIndexOutOfBounds(rawId)           ((g_RawIndexPastEnd & g_RawAccessOutOfBoundsMask[rawId]) > 0)

/*
 * Raw registers for each raw window being used
 */
static const u_int8_t g_RawIndexReg[NUM_RAW_WINDOWS] = {
    MRF24_REG_RAW0_INDEX,
    MRF24_REG_RAW1_INDEX,
    MRF24_REG_RAW2_INDEX,
    MRF24_REG_RAW3_INDEX,
    MRF24_REG_RAW4_INDEX,
    MRF24_REG_RAW5_INDEX,
};
static const u_int8_t g_RawStatusReg[NUM_RAW_WINDOWS] = {
    MRF24_REG_RAW0_STATUS,
    MRF24_REG_RAW1_STATUS,
    MRF24_REG_RAW2_STATUS,
    MRF24_REG_RAW3_STATUS,
    MRF24_REG_RAW4_STATUS,
    MRF24_REG_RAW5_STATUS,
};
static const u_int16_t g_RawCtrl0Reg[NUM_RAW_WINDOWS] = {
    MRF24_REG_RAW0_CTRL0,
    MRF24_REG_RAW1_CTRL0,
    MRF24_REG_RAW2_CTRL0,
    MRF24_REG_RAW3_CTRL0,
    MRF24_REG_RAW4_CTRL0,
    MRF24_REG_RAW5_CTRL0,
};
static const u_int16_t g_RawCtrl1Reg[NUM_RAW_WINDOWS] = {
    MRF24_REG_RAW0_CTRL1,
    MRF24_REG_RAW1_CTRL1,
    MRF24_REG_RAW2_CTRL1,
    MRF24_REG_RAW3_CTRL1,
    MRF24_REG_RAW4_CTRL1,
    MRF24_REG_RAW5_CTRL1,
};
static const u_int16_t g_RawDataReg[NUM_RAW_WINDOWS] = {
    MRF24_REG_RAW0_DATA,
    MRF24_REG_RAW1_DATA,
    MRF24_REG_RAW2_DATA,
    MRF24_REG_RAW3_DATA,
    MRF24_REG_RAW4_DATA,
    MRF24_REG_RAW5_DATA,
};

/*
 * Interrupt mask for each raw window; note that raw0 and raw1 are really
 * 8 bit values and will be cast when used.
 */
static const u_int16_t g_RawIntMask[NUM_RAW_WINDOWS] = {
    INTR_RAW0,      /* used in HOST_INTR reg (8-bit register) */
    INTR_RAW1,      /* used in HOST_INTR reg (8-bit register) */
    INTR2_RAW2,     /* used in HOST_INTR2 reg (16-bit register) */
    INTR2_RAW3,     /* used in HOST_INTR2 reg (16-bit register) */
    INTR2_RAW4,     /* used in HOST_INTR2 reg (16-bit register) */
    INTR2_RAW5,     /* used in HOST_INTR2 reg (16-bit register) */
};

/* keeps track of whether raw tx/rx data windows mounted or not */
static u_int8_t RawWindowState[2];  // [0] is RAW Rx window, [1] is RAW Tx window

const u_int8_t g_RawAccessOutOfBoundsMask[NUM_RAW_WINDOWS] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20
};
static u_int8_t g_RawIndexPastEnd = 0;  /* no indexes are past end of window */

/*
 * Wait for a RAW move to complete.
 * Returns a number of bytes that were overlayed (not always applicable).
 */
static unsigned WaitForRawMoveComplete(unsigned rawId)
{
    unsigned intr, intMask;
    unsigned nbytes, regId, start_time;

    /* create mask to check against for Raw Move complete interrupt for either RAW0 or RAW1 */
    if (rawId <= 1) {
        /* will be either raw 0 or raw 1 */
        intMask = (rawId == 0) ? INTR_RAW0 : INTR_RAW1;
    } else {
        /* will be INTR2 bit in host register, signifying RAW2, RAW3, or RAW4 */
        intMask = INTR_INT2;
    }

    start_time = mrf_timer_read();
    for (;;) {
        intr = mrf_read_byte(MRF24_REG_INTR);

        /* If received an external interrupt that signaled the RAW Move
         * completed then break out of this loop. */
        if (intr & intMask) {
            /* clear the RAW interrupts, re-enable interrupts, and exit */
            if (intMask == INTR_INT2)
                mrf_write(MRF24_REG_INTR2,
                    INTR2_RAW2 | INTR2_RAW3 | INTR2_RAW4 | INTR2_RAW5);
            mrf_write_byte(MRF24_REG_INTR, intMask);
            break;
        }

        if (mrf_timer_elapsed(start_time) > 20) {
            printf("--- %s: timeout waiting for interrupt\n", __func__);
            break;
        }
        udelay(10);
    }

    /* read the byte count and return it */
    regId = g_RawCtrl1Reg[rawId];
    nbytes = mrf_read(regId);

    return nbytes;
}

/*
 * Initialize RAW (Random Access Window) on MRF24WG
 */
void RawInit()
{
    // By default the MRF24WG firmware mounts Scratch to RAW 1 after reset. This
    // is not being used, so unmount the scratch from this RAW window.
    ScratchUnmount(1);

    /* Permanently mount scratch memory, index defaults to 0. */
    /* If one needs to know, this function returns the number of bytes in scratch memory */
    ScratchMount(RAW_SCRATCH_ID);

    SetRawDataWindowState(RAW_DATA_TX_ID, WF_RAW_UNMOUNTED);
    SetRawDataWindowState(RAW_DATA_RX_ID, WF_RAW_UNMOUNTED);
}

/*
 * Mounts RAW scratch window.
 * Returns size, in bytes, of Scratch buffer.
 *
 * The scratch window is not dynamically allocated, but references a static
 * portion of the WiFi device RAM. Thus, the Scratch data is not lost when
 * the scratch window is unmounted.
 *
 * Parameters:
 *  rawId -- RAW window ID being used to mount the scratch data
 */
u_int16_t ScratchMount(u_int8_t rawId)
{
    u_int16_t nbytes;

    nbytes = RawMove(rawId, RAW_SCRATCH_POOL, 1, 0);
    return nbytes;
}

/*
 * Unmount RAW scratch window.
 * Returns size, in bytes, of Scratch buffer.
 *
 * The scratch window is not dynamically allocated, but references a static
 * portion of the WiFi device RAM. Thus, the Scratch data is not lost when
 * the scratch window is unmounted.
 *
 * Parameters:
 *  rawId -- RAW window ID that was used to mount the scratch window
 */
void ScratchUnmount(u_int8_t rawId)
{
    RawMove(rawId, RAW_SCRATCH_POOL, 0, 0);
}

/*
 * Allocate a Mgmt Tx buffer.
 * Returns True if mgmt tx buffer successfully allocated, else False.
 *
 * Determines if WiFi chip has enough memory to allocate a tx mgmt buffer, and,
 * if so, allocates it.
 *
 * Parameters:
 *  bytesNeeded -- number of bytes needed for the mgmt tx message
 */
bool AllocateMgmtTxBuffer(u_int16_t bytesNeeded)
{
    u_int16_t bufAvail;
    u_int16_t nbytes;

    /* get total bytes available for MGMT tx memory pool */
    bufAvail = mrf_read(MRF24_REG_WFIFO_BCNT1) & FIFO_BCNT_MASK;

    /* if enough bytes available to allocate */
    if (bufAvail >= bytesNeeded) {
        /* allocate and create the new Mgmt Tx buffer */
        nbytes = RawMove(RAW_MGMT_TX_ID, RAW_MGMT_POOL, 1, bytesNeeded);
        if (nbytes == 0) {
            /*printf("--- %s: cannot allocate %u bytes of %u free\n",
                __func__, bytesNeeded, bufAvail); */
            return 0;
        }
        ClearIndexOutOfBoundsFlag(RAW_MGMT_TX_ID);
        return 1;
    }
    /* else not enough bytes available at this time to satisfy request */
    else {
        /* if we allocated some bytes, but not enough, then deallocate what was allocated */
        if (bufAvail > 0) {
            RawMove(RAW_MGMT_RX_ID, RAW_MGMT_POOL, 0, 0);
        }
        return 0;
    }
}

/*
 * Deallocates a mgmt Rx buffer
 * Called by WiFi driver when its finished processing a Rx mgmt message.
 */
void DeallocateMgmtRxBuffer()
{
    /* Unmount (release) mgmt packet now that we are done with it */
    RawMove(RAW_MGMT_RX_ID, RAW_MGMT_POOL, 0, 0);
}

/*
 * Write bytes to RAW window.
 * Parameters:
 *  rawId   - RAW ID
 *  pBuffer - Buffer containing bytes to write
 *  length  - number of bytes to read
 */
void RawSetByte(u_int16_t rawId, const u_int8_t *p_buffer, u_int16_t length)
{
    u_int8_t regId;

    // if trying to write past end of raw window
    if (isIndexOutOfBounds(rawId)) {
        printf("--- %s: index out of bounds\n", __func__);
    }

    /* write data to raw window */
    regId = g_RawDataReg[rawId];
    mrf_write_array(regId, p_buffer, length);
}

/*
 * Read bytes from the specified raw window.
 * Returns error code.
 *
 * Parameters:
 *  rawId   - RAW ID
 *  pBuffer - Buffer to read bytes into
 *  length  - number of bytes to read
 */
void RawGetByte(u_int16_t rawId, u_int8_t *pBuffer, u_int16_t length)
{
    u_int8_t regId;

    // if the raw index was previously set out of bounds
    if (isIndexOutOfBounds(rawId)) {
        // trying to read past end of raw window
        printf("--- %s: index out of bounds\n", __func__);
    }

    regId = g_RawDataReg[rawId];
    mrf_read_array(regId, pBuffer, length);
}

/*
 * Sends a management frame to the WiFi chip.
 *
 * The management header, which consists of a type and subtype, have already
 * been written to the frame before this function is called.
 *
 * Parameters:
 *  bufLen -- number of bytes that comprise the management frame.
 */
void SendRAWManagementFrame(u_int16_t bufLen)
{
    /* Notify WiFi device that management message is ready to be processed */
    RawMove(RAW_MGMT_TX_ID, RAW_MAC, 0, bufLen);
}

/*
 * Mounts most recent Rx message.
 * Returns length, a number of bytes in the received message.
 *
 * This function mounts the most recent Rx message from the WiFi chip, which
 * could be either a management or a data message.
 *
 * Parameters:
 *  rawId -- RAW ID specifying which raw window to mount the rx packet in.
 */
u_int16_t RawMountRxBuffer(u_int8_t rawId)
{
    u_int16_t length;

    length = RawMove(rawId, RAW_MAC, 1, 0);

    // the length should never be 0 if notified of an Rx msg
    if (length == 0) {
        printf("--- %s: failed\n", __func__);
    }

    /* if mounting a Raw Rx data frame */
    if (rawId == RAW_DATA_RX_ID) {
        /* notify WiFi driver that an Rx data frame is mounted */
        SetRawDataWindowState(RAW_DATA_RX_ID, WF_RAW_DATA_MOUNTED);
    }
    return length;
}

/*
 * Read the specified number of bytes from a mounted RAW window
 * from the specified starting index.
 * Returns error code.
 *
 * Parameters:
 *  rawId      -- RAW window ID being read from
 *  startIndex -- start index within RAW window to read from
 *  length     -- number of bytes to read from the RAW window
 *  p_dest     -- pointer to Host buffer where read data is copied
 */
void RawRead(u_int8_t rawId, u_int16_t startIndex, u_int16_t length, u_int8_t *p_dest)
{
    RawSetIndex(rawId, startIndex);
    RawGetByte(rawId, p_dest, length);
}

/*
 * Write the specified number of bytes to a mounted RAW window
 * at the specified starting index.
 *
 * Parameters:
 *  rawId      -- RAW window ID being written to
 *  startIndex -- start index within RAW window to write to
 *  length     -- number of bytes to write to RAW window
 *  p_src      -- pointer to Host buffer write data
 */
void RawWrite(u_int8_t rawId, u_int16_t startIndex, u_int16_t length, const u_int8_t *p_src)
{
    /* set raw index in dest memory */
    RawSetIndex(rawId, startIndex);

    /* write data to RAW window */
    RawSetByte(rawId, p_src, length);
}

/*
 * Sets the index within the specified RAW window.
 *
 * Sets the index within the specified RAW window. If attempt to set RAW index
 * outside boundaries of RAW window (past the end) this function will time out.
 * It's legal to set the index past the end of the raw window so long as there
 * is no attempt to read or write at that index.  For now, flag an event.
 *
 * Parameters:
 *  rawId -- RAW window ID
 *  index -- desired index within RAW window
 */
void RawSetIndex(u_int16_t rawId, u_int16_t index)
{
    u_int8_t  regId;
    u_int16_t regValue;
    u_int32_t start_time;

    /* get the index register associated with the raw ID and write to it */
    regId = g_RawIndexReg[rawId];
    mrf_write(regId, index);

    /* Get the raw status register address associated with the raw ID.  This will be polled to
     * determine that:
     *  1) raw set index completed successfully  OR
     *  2) raw set index failed, implying that the raw index was set past the end of the raw window
     */
    regId = g_RawStatusReg[rawId];

    /* read the status register until set index operation completes or times out */
    start_time = mrf_timer_read();
    while (1) {
        regValue = mrf_read(regId);
        if ((regValue & WF_RAW_STATUS_REG_BUSY_MASK) == 0) {
            ClearIndexOutOfBoundsFlag(rawId);
            break;
        }

        if (mrf_timer_elapsed(start_time) > 5) {
            // if we timed out that means that the caller is trying to set the index
            // past the end of the raw window.  Not illegal in of itself so long
            // as there is no attempt to read or write at this location.  But,
            // applications should avoid this to avoid the timeout in
            SetIndexOutOfBoundsFlag(rawId);
            printf("--- %s: bad index=%u out of bounds\n", __func__, index);
            break;
        }
        udelay(10);
    }
}

/*
 * Allocate a Data Tx buffer for use by the TCP/IP stack.
 * Returns True if data tx buffer successfully allocated, else False.
 *
 * Determines if WiFi chip has enough memory to allocate a tx data buffer,
 * and, if so, allocates it.
 *
 * Parameters:
 *  bytesNeeded -- number of bytes needed for the data tx message
 */
bool AllocateDataTxBuffer(u_int16_t bytesNeeded)
{
    u_int16_t bufAvail;
    u_int16_t nbytes;

    /* get total bytes available for DATA tx memory pool */
    bufAvail = mrf_read(MRF24_REG_WFIFO_BCNT0) & FIFO_BCNT_MASK;
    if (bufAvail < bytesNeeded) {
        /* not enough bytes available at this time to satisfy request */
        return 0;
    }

    /* allocate and create the new Tx buffer (mgmt or data) */
    nbytes = RawMove(RAW_DATA_TX_ID, RAW_DATA_POOL, 1, bytesNeeded);
    if (nbytes == 0) {
        printf("--- %s: failed\n", __func__);
        return 0;
    }

    /* flag this raw window as mounted (in use) */
    SetRawDataWindowState(RAW_DATA_TX_ID, WF_RAW_DATA_MOUNTED);
    return 1;
}

/*
 * Deallocate a Data Rx buffer.
 *
 * Typically called by MACGetHeader(), the assumption being that when
 * the stack is checking for a newly received data message it is finished
 * with the previously received data message.  Also called by MACGetHeader()
 * if the SNAP header is invalid and the packet is thrown away.
 */
void DeallocateDataRxBuffer()
{
    // TODO: verify data rx is mounted

    SetRawDataWindowState(RAW_DATA_RX_ID, WF_RAW_UNMOUNTED);

    /* perform deallocation of raw rx buffer */
    RawMove(RAW_DATA_RX_ID, RAW_DATA_POOL, 0, 0);
}

/*
 * Perform RAW Move operation.
 * When applicable, return the number of bytes overlayed by the raw move.
 *
 * The function performs a variety of operations (e.g. allocating tx buffers,
 * mounting rx buffers, copying from one raw window to another, etc.)
 *
 * Parameters:
 *  rawId -- Raw ID 0 thru 5, except is srcDest is RAW_COPY, in which case rawId
 *           contains the source address in the upper 4 bits and destination
 *           address in lower 4 bits.
 *
 *  srcDest -- object that will either be the source or destination of the move:
 *                RAW_MAC
 *                RAW_MGMT_POOL
 *                RAW_DATA_POOL
 *                RAW_SCRATCH_POOL
 *                RAW_STACK_MEM
 *                RAW_COPY (this object not allowed, handled in RawToRawCopy())
 *
 *  rawIsDestination -- true is srcDest is the destination, false if srcDest is
 *                      the source of the move
 *
 *  size -- number of bytes to overlay (not always applicable)
 */
u_int16_t RawMove(u_int16_t rawId,
                 u_int16_t srcDest,
                 bool     rawIsDestination,
                 u_int16_t size)
{
    u_int16_t nbytes;
    u_int8_t  regId;
    u_int8_t  regValue;
    u_int16_t ctrlVal = 0;
    bool intEnabled;

    // save current state of External interrupt and disable it
    intEnabled = mrf_intr_disable();

    /* Create control value that will be written to raw control register,
     * which initiates the raw move */
    if (rawIsDestination) {
        ctrlVal |= 0x8000;
    }
    /* fix later, simply need to ensure that size is 12 bits are less */
    ctrlVal |= (srcDest << 8);              /* defines are already shifted by 4 bits */
    ctrlVal |= ((size >> 8) & 0x0f) << 8;   /* MS 4 bits of size (bits 11:8) */
    ctrlVal |= (size & 0x00ff);             /* LS 8 bits of size (bits 7:0) */

    /*
     * This next 'if' block is used to ensure the expected raw interrupt
     * signifying raw move complete is cleared.
     */

    /* if doing a raw move on Raw 0 or 1 (data rx or data tx) */
    if (rawId <= 1) {
        /* Clear the interrupt bit in the host interrupt register (Raw 0 and 1 are in 8-bit host intr reg) */
        regValue = (u_int8_t)g_RawIntMask[rawId];
        mrf_write_byte(MRF24_REG_INTR, regValue);
    }
    /* else doing raw move on mgmt rx, mgmt tx, or scratch */
    else {
        /* Clear the interrupt bit in the host interrupt 2 register (Raw 2,3, and 4 are in 16-bit host intr2 reg */
        regValue = g_RawIntMask[rawId];
        mrf_write(MRF24_REG_INTR2, regValue);
    }

    /*
     * Now that the expected raw move complete interrupt has been cleared
     * and we are ready to receive it, initiate the raw move operation by
     * writing to the appropriate RawCtrl0.
     */
    regId = g_RawCtrl0Reg[rawId];                   /* get RawCtrl0 register address for desired raw ID */
    mrf_write(regId, ctrlVal);                      /* write ctrl value to register */

    // enable interrupts so we get raw move complete interrupt
    mrf_intr_enable();
    nbytes = WaitForRawMoveComplete(rawId);      /* wait for raw move to complete */

    // if interrupts were disabled coming into this function, put back to that state
    if (! intEnabled) {
        mrf_intr_disable();
    }

    /* byte count is not valid for all raw move operations */
    return nbytes;
}

/* sets and gets the state of RAW data tx/rx windows */
void SetRawDataWindowState(u_int8_t rawId, u_int8_t state)
{
    RawWindowState[rawId] = state;
}

u_int8_t GetRawDataWindowState(u_int8_t rawId)
{
    return RawWindowState[rawId];
}