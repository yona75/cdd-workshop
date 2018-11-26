/*
 * Amazon FreeRTOS Shadow Demo V1.2.3
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */


#include "shadow_task.h"


/* FreeRTOS includes */
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "queue.h"
/* AWS libs */
#include "aws_clientcredential.h"
#include "aws_shadow.h"
#include "aws_mqtt_agent.h"

#include <string.h>

#include "semphr.h"
#include "json.h"
#include "driver/gpio.h"
#include "motor.h"
#include "motor_task.h"

/* Timeout for connection to the MQTT broker (300 milliseconds) */
#define MQTT_TIMEOUT                  ( 150 )
/* Send buffer size */
#define SEND_BUFFER_SIZE              256
/* Maximum number of tokens that can be contained in a JSON document */
#define MAX_JSON_TOKENS               172
/* Wait timeout for update queue */
#define UPDATE_TIMEOUT                50
/* Update queue size */
#define UPDATE_QUEUE_SIZE             5
/* Defines for parsing helpers */
#define NO_SUCH_KEY                   ( -1 )

#define DEFAULT_DISPOSE_DURATION      2

#define SHADOW_UPDATE_PERIOD_MS       1 * 1000
#define SHADOW_UPDATE_PERIOD_TICKS    ( SHADOW_UPDATE_PERIOD_MS * configTICK_RATE_HZ / 1000 )


/* Handle of the shadwow client */
static ShadowClientHandle_t xClientHandle;

/* An element of the Shadow update queue. */

static QueueHandle_t xUpdateQueue = NULL;

static int last_version = -1;

char buffer[ SEND_BUFFER_SIZE ];


static struct State current_state = { false, DEFAULT_DISPOSE_DURATION };

/* Template for the reported JSON */
static const char shadowReportJSon[] =
    "{"
    "\"state\":{"
    "\"reported\":{"
    "\"power_state\": %d,"
    "\"duration\": %d"
    "},"
    "\"desired\":{"
    "%s"
    "}"
    "},"
    "\"clentToken\": \"token-%d\""
    "}";

QueueHandle_t resetSema;

void prvUpdateShadow( struct State new_state );

void prvGenerateReport( char * buffer,
                        size_t buffersize,
                        struct State s,
                        char * desired )
{
    snprintf( buffer, buffersize, shadowReportJSon, s.power_state, s.duration, desired,
              ( int ) xTaskGetTickCount() );
}

static void changeState( struct State new_state )
{
    current_state.duration = new_state.duration; /* We are not going to change duration of already running operation, but all further will use the new duration */

    if( current_state.power_state == new_state.power_state )
    {
        return; /* We don't want to do anything in the case states are equal*/
    }

    current_state.power_state = new_state.power_state;
    xQueueSend( motorStatesQueue, &current_state, 0 );
}

static json_object_entry * find_key( json_value * o,
                                     const char * key )
{
    for( int i = 0; i < o->u.object.length; ++i )
    {
        if( strcmp( o->u.object.values[ i ].name, key ) == 0 )
        {
            return &o->u.object.values[ i ];
        }
    }

    printf( "key not found: %s\n", key );
    return NULL;
}

static BaseType_t prvDeltaCallback( void * pvUserData,
                                    const char * const pcThingName,
                                    const char * const pcDeltaDocument,
                                    uint32_t ulDocumentLength,
                                    MQTTBufferHandle_t xBuffer )
{
    ( void ) xBuffer;
    ( void ) pvUserData;
    ( void ) pcThingName;
    printf( "DELTA: %s\n", pcDeltaDocument );
    struct State new_state = current_state; /* We need a correct initial value */
    json_value * val = json_parse( pcDeltaDocument, ulDocumentLength );
    json_object_entry * state = find_key( val, "state" );

    if( !state )
    {
        json_value_free( val );
        printf( "WHAT\n" );
        return pdTRUE;
    }

    json_object_entry * power_state = find_key( state->value, "power_state" );
    json_object_entry * duration = find_key( state->value, "duration" );

    if( duration )
    {
        new_state.duration = duration->value->u.integer;
    }

    if( power_state )
    {
        new_state.power_state = power_state->value->u.integer;
    }

    changeState( new_state );
    json_value_free( val );
    SHADOW_ReturnMQTTBuffer( xClientHandle, xBuffer );
    return pdTRUE;
}



static ShadowReturnCode_t prvGetState()
{
    ShadowOperationParams_t xOperationParams;
    ShadowReturnCode_t xReturn;
    struct State new_state;

    memset( &xOperationParams, 0, sizeof( xOperationParams ) );
    xOperationParams.pcThingName = clientcredentialIOT_THING_NAME;
    xOperationParams.xQoS = 1;
    printf( "Shadow Get \n" );
    xReturn = SHADOW_Get( xClientHandle, &xOperationParams, MQTT_TIMEOUT * 3 );
    printf( "Shadow Get return: %d\n", xReturn );

    if( xReturn != eShadowSuccess )
    {
        return xReturn;
    }

    printf( "%s %d\n", xOperationParams.pcData, xOperationParams.ulDataLength );
    json_value * val = json_parse( xOperationParams.pcData, xOperationParams.ulDataLength );
    json_object_entry * state = find_key( val, "state" );

    if( !state )
    {
        json_value_free( val );
        return pdTRUE;
    }

    json_object_entry * reported = find_key( state->value, "reported" );

    if( !reported )
    {
        json_value_free( val );
        return pdTRUE;
    }

    json_object_entry * power_state = find_key( reported->value, "power_state" );

    if( !power_state )
    {
        json_value_free( val );
        return pdTRUE;
    }

    new_state.power_state = power_state->value->u.integer;
    json_object_entry * duration = find_key( reported->value, "duration" );

    if( !duration )
    {
        new_state.duration = DEFAULT_DISPOSE_DURATION;
    }
    else
    {
        new_state.duration = duration->value->u.integer;
    }

    printf( "G New State: %d\n", new_state.power_state );
    changeState( new_state );
    json_value_free( val );
    SHADOW_ReturnMQTTBuffer( xClientHandle, xOperationParams.xBuffer );
    return xReturn;
}

ShadowReturnCode_t prvConnectToShadow( void )
{
    ShadowCreateParams_t xCreateParam;
    ShadowReturnCode_t xReturn;
    MQTTAgentConnectParams_t xConnectParams;

    xCreateParam.xMQTTClientType = eDedicatedMQTTClient;
    printf( "SHADOW_ClientConnect ...\n" );
    xReturn = SHADOW_ClientCreate( &xClientHandle, &xCreateParam );

    if( xReturn == eShadowSuccess )
    {
        memset( &xConnectParams, 0, sizeof( xConnectParams ) );
        xConnectParams.pcURL = clientcredentialMQTT_BROKER_ENDPOINT;
        xConnectParams.usPort = clientcredentialMQTT_BROKER_PORT;
        xConnectParams.xFlags =
            mqttagentREQUIRE_TLS;
        xConnectParams.pcCertificate = NULL;
        xConnectParams.ulCertificateSize = 0;
        xConnectParams.pvUserData = &xClientHandle;
        xConnectParams.pucClientId = ( const unsigned char * )
                                     clientcredentialIOT_THING_NAME;
        xConnectParams.usClientIdLength = strlen( clientcredentialIOT_THING_NAME );
        xReturn = SHADOW_ClientConnect( xClientHandle, &xConnectParams, MQTT_TIMEOUT * 3 );

        if( xReturn != eShadowSuccess )
        {
            printf( "SHADOW_ClientConnect unsuccessful: %d\n", xReturn );
        }
    }
    else
    {
        printf( "SHADOW_ClientCreate unsuccessful: %d\n", xReturn );
    }

    return xReturn;
}

void xShadowTask( void * param )
{
    ( void ) param;
    ShadowReturnCode_t xReturn;
    ShadowCallbackParams_t xCallbackParams;
    ShadowOperationParams_t xUpdateParam;
    resetSema = xSemaphoreCreateBinary();
    xReturn = prvConnectToShadow();
    TickType_t xLastWakeTime;

    if( xReturn == eShadowSuccess )
    {
        /* Set the callbacks sent by the IoT shadow */
        xCallbackParams.pcThingName = clientcredentialIOT_THING_NAME;
        xCallbackParams.xShadowDeletedCallback = NULL;
        xCallbackParams.xShadowDeltaCallback = prvDeltaCallback;
        xCallbackParams.xShadowUpdatedCallback = NULL;
        xReturn = SHADOW_RegisterCallbacks( xClientHandle, &xCallbackParams, MQTT_TIMEOUT * 3 );

        if( xReturn == eShadowSuccess )
        {
            /* Receive the current shadow state */
            xReturn = prvGetState();
            /*            if (xReturn != eShadowSuccess) return; */

            xLastWakeTime = xTaskGetTickCount();

            for( ; ; )
            {
                if( xSemaphoreTake( motorStopSema, 0 ) == pdTRUE ) /* reset desired */
                {
                    current_state.power_state = 0;
                    prvGenerateReport( buffer,
                                       SEND_BUFFER_SIZE,
                                       current_state, "\"power_state\": 0" );
                }
                else
                {
                    prvGenerateReport( buffer,
                                       SEND_BUFFER_SIZE,
                                       current_state, "" );
                }

                printf( "%s", buffer );
                xUpdateParam.pcThingName = clientcredentialIOT_THING_NAME;
                xUpdateParam.xQoS = eMQTTQoS0;
                xUpdateParam.pcData = buffer;
                xUpdateParam.ucKeepSubscriptions = pdTRUE;
                /* Generate data for sending to the shadow */
                xUpdateParam.ulDataLength = ( uint32_t ) strlen( buffer );

                xReturn = SHADOW_Update( xClientHandle, &xUpdateParam, MQTT_TIMEOUT * 2 );

                if( xReturn == eShadowSuccess )
                {
                    configPRINTF( ( "Successfully performed update.\r\n" ) );
                }
                else
                {
                    configPRINTF( ( "Update failed, returned %d.\r\n", xReturn ) );
                }

                printf( "Free mem: %d\n", xPortGetFreeHeapSize() );
                vTaskDelayUntil( &xLastWakeTime, SHADOW_UPDATE_PERIOD_TICKS );
            }
        }
    }
}
