#include "motor_task.h"
#include "task.h"
#include "shadow_task.h"
#include "stdio.h"
#include "led_strip/led_strip.h"
#include "stdint.h"
#include "motor.h"

#define LEDS_GPIO                 5
#define LED_STRIP_LENGTH          8U
#define LED_STRIP_RMT_INTR_NUM    19
#define LED_CHANGE_MS             100
#define LED_CHANGE_TICKS          ( LED_CHANGE_MS * configTICK_RATE_HZ / 1000 )
static struct led_color_t led_strip_buf_1[ LED_STRIP_LENGTH ];
static struct led_color_t led_strip_buf_2[ LED_STRIP_LENGTH ];

QueueHandle_t motorStatesQueue;
SemaphoreHandle_t motorStopSema;

void prvLedTurnOff( struct led_strip_t * led_strip,
                    int num )
{
    for( int i = 0; i < num; ++i ) /** Turn off the LEDs */
    {
        led_strip_set_pixel_rgb( led_strip, i, 0, 0, 0 );
    }

    led_strip_show( led_strip );
}

void xMotorTask( void * param )
{
    motorStatesQueue = xQueueCreate( 1, sizeof( struct State ) );

    struct State new_state;
    BaseType_t status;

    struct led_strip_t led_strip =
    {
        .rgb_led_type      = RGB_LED_TYPE_WS2812,
        .rmt_channel       = RMT_CHANNEL_1,
        .rmt_interrupt_num = LED_STRIP_RMT_INTR_NUM,
        .gpio              = LEDS_GPIO,
        .led_strip_buf_1   = led_strip_buf_1,
        .led_strip_buf_2   = led_strip_buf_2,
        .led_strip_length  = LED_STRIP_LENGTH
    };
    led_strip.access_semaphore = xSemaphoreCreateBinary();
    printf( "led_strip.access_semaphore is %x \n", ( size_t ) led_strip.access_semaphore );

    motorStopSema = xSemaphoreCreateBinary();
    bool ok = led_strip_init( &led_strip );
    printf( "Led strip initialized: %d\n", ok );
    hsv hsv_color = { 0, 1, 1 };
    hsv hsv_color_next = { 0, 1, 0.01 };
    rgb rgb_color = { 0, 0, 0 };
    rgb rgb_color_next = { 0, 0, 0 };
    uint32_t old_ticks = xTaskGetTickCount();
    int ticks_remained = 0;
    bool need_stop = false; /* Flag indicating whether we stops by timeout or not */
    prvLedTurnOff( &led_strip, LED_STRIP_LENGTH );

    int i = 0;

    for( ; ; )
    {
        printf( "::: Motor_task tick, ticks_ramained = %d\n", ticks_remained );

        if( ticks_remained == 0 )
        {
            if( !need_stop )
            {
                /* Motor currently is off, we wait for a command from the shadow */
                status = xQueueReceive( motorStatesQueue, &new_state, portMAX_DELAY );

                if( ( status == pdTRUE ) && ( new_state.power_state == 1 ) )
                {
                    ticks_remained = new_state.duration * configTICK_RATE_HZ;
                    motor_forward( DEFAULT_MOTOR );
                    printf( "Start the freaking motor! Ticks_remained = %d\n", ticks_remained );
                    old_ticks = xTaskGetTickCount(); /* We need it to bypass situation where a lot of tick has passed before we received a semaphore */
                }
            }
        }
        else /* We received command to stop a motor */
        {
            printf( "::: Motor_task before receive during work\n" );
            status = xQueueReceive( motorStatesQueue, &new_state, 0 );
            printf( "::: Motor_task after receive during work\n" );

            if( ( status == pdTRUE ) && ( new_state.power_state == 0 ) )
            {
                ticks_remained = 0;
                motor_brake( DEFAULT_MOTOR );
                printf( "Forceful stop the motor\n" );
            }
        }

        if( ( ticks_remained == 0 ) && need_stop )
        {
            prvLedTurnOff( &led_strip, LED_STRIP_LENGTH );
            motor_brake( DEFAULT_MOTOR );
            printf( "Stop the motor by time passed\n" );
            xSemaphoreGive( motorStopSema );
            need_stop = false;
        }
        else
        {
            printf( "led_strip.access_semaphore is %x \n", ( size_t ) led_strip.access_semaphore );
            hsv_color.h = 4 * i;
            hsv_color_next.h = 4 * i;
            rgb_color = hsv2rgb( hsv_color );
            rgb_color_next = hsv2rgb( hsv_color_next );
            led_strip_set_pixel_rgb( &led_strip, i % 8, rgb_color.r * 255, rgb_color.g * 255, rgb_color.b * 255 );
            led_strip_set_pixel_rgb( &led_strip, ( i + 7 ) % 8, rgb_color_next.r * 255, rgb_color_next.g * 255, rgb_color_next.b * 255 );
            led_strip_set_pixel_rgb( &led_strip, ( i + 1 ) % 8, rgb_color_next.r * 255, rgb_color_next.g * 255, rgb_color_next.b * 255 );

            led_strip_show( &led_strip );
            i = ( i + 1 ) % 80;
            int ticks_passed = ( xTaskGetTickCount() - old_ticks );
            printf( "::: Motor_task ticks_passed = %d\n", ticks_passed );
            ticks_remained = ticks_remained - ticks_passed;
            ticks_remained = ticks_remained < 0 ? 0 : ticks_remained; /* Clamp it from below by zero */

            if( ticks_remained == 0 )
            {
                need_stop = true;
            }
        }

        old_ticks = xTaskGetTickCount();
        printf( "::: Motor_task before delay, old_ticks = %d\n", old_ticks );
        vTaskDelay( LED_CHANGE_TICKS );
    }
}
