#ifndef DEMOS_H
#define DEMOS_H

/* Tarea de comandos y demos interactivas (inversion, prioridades...).
 * vInversionDemoTask no se declara aqui: es privada de demos.c, la crea
 * la propia tarea de comandos con la tecla 'i'. */

/* Ayuda de teclas que imprime la tarea de comandos ('?') y el banner. */
extern const char * pcHelpText;

void vCommandTask( void * pvParameters );
void vInvLowTask( void * pvParameters );
void vInvMedTask( void * pvParameters );
void vInvHighTask( void * pvParameters );

#endif
