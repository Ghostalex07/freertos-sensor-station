#ifndef DEMOS_H
#define DEMOS_H

/* Command task and interactive demos (inversion, priorities...).
 * vInversionDemoTask is not declared here: it is private to demos.c and
 * the command task itself creates it with the 'i' key. */

/* Key help printed by the command task ('?') and the banner. */
extern const char * pcHelpText;

void vCommandTask( void * pvParameters );
void vInvLowTask( void * pvParameters );
void vInvMedTask( void * pvParameters );
void vInvHighTask( void * pvParameters );

#endif
