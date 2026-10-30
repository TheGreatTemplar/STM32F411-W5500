; Arm Compiler 6 / ARMASM startup with USART1 and DMA2 RX interrupts.
Stack_Size      EQU     0x00001000
Heap_Size       EQU     0x00000200

                AREA    STACK, NOINIT, READWRITE, ALIGN=3
Stack_Mem       SPACE   Stack_Size
__initial_sp
                AREA    HEAP, NOINIT, READWRITE, ALIGN=3
__heap_base
Heap_Mem        SPACE   Heap_Size
__heap_limit

                PRESERVE8
                THUMB
                AREA    RESET, DATA, READONLY
                EXPORT  __Vectors
                EXPORT  __Vectors_End
                EXPORT  __Vectors_Size
__Vectors
                DCD     __initial_sp
                DCD     Reset_Handler
                DCD     NMI_Handler
                DCD     Default_Handler     ; HardFault
                DCD     Default_Handler     ; MemManage
                DCD     Default_Handler     ; BusFault
                DCD     Default_Handler     ; UsageFault
                DCD     0, 0, 0, 0
                DCD     Default_Handler     ; SVC
                DCD     Default_Handler     ; DebugMonitor
                DCD     0
                DCD     Default_Handler     ; PendSV
                DCD     SysTick_Handler
                GBLA    irq_index
irq_index       SETA    0
                WHILE   irq_index < 86
                IF      irq_index = 37
                DCD     USART1_IRQHandler
                ELSE
                IF      irq_index = 58
                DCD     DMA2_Stream2_IRQHandler
                ELSE
                DCD     Default_Handler
                ENDIF
                ENDIF
irq_index       SETA    irq_index + 1
                WEND
__Vectors_End
__Vectors_Size  EQU     __Vectors_End - __Vectors

                AREA    |.text|, CODE, READONLY
Reset_Handler   PROC
                EXPORT  Reset_Handler
                IMPORT  SystemInit
                IMPORT  NMI_Handler
                IMPORT  __main
                IMPORT  SysTick_Handler
                IMPORT  USART1_IRQHandler
                IMPORT  DMA2_Stream2_IRQHandler
                LDR     R0, =SystemInit
                BLX     R0
                LDR     R0, =__main
                BX      R0
                ENDP

Default_Handler PROC
                EXPORT  Default_Handler
                B       .
                ENDP

                EXPORT  __initial_sp
                EXPORT  __heap_base
                EXPORT  __heap_limit
                ALIGN
                END
