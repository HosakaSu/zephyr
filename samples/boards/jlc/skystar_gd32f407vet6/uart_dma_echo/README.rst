.. zephyr:code-sample:: skystar-gd32f407vet6-uart-dma-echo
   :name: SkyStar GD32F407VET6 UART DMA echo
   :relevant-api: uart_interface

   Echo bytes through USART0 using the Zephyr UART asynchronous API and GD32 DMA.

Overview
********

This sample enables DMA-backed asynchronous RX and TX on USART0 of the
SkyStar GD32F407VET6 board. Received bytes are queued and sent back on the
same UART.

Building and Running
********************

Build the sample for the SkyStar GD32F407VET6 board:

.. zephyr-app-commands::
   :zephyr-app: samples/boards/jlc/skystar_gd32f407vet6/uart_dma_echo
   :board: skystar_gd32f407vet6
   :goals: build flash
   :compact:

Open the board serial port at 115200 baud. Any bytes sent to the board are
echoed back.
