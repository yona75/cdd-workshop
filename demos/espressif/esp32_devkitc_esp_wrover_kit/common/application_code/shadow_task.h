#ifndef SHADOW_TASK_H
#define SHADOW_TASK_H

#include "stdint.h"
#include "stdbool.h"

void xShadowTask( void * param );

struct State
{
    bool power_state;
    uint32_t duration;
};


#endif /* SHADOW_TASK_H */
