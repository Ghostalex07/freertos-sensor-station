#include "logic.h"

#include <stdio.h>
#include <string.h>

static int iFailures = 0;

#define CHECK( cond, msg )                          \
    do                                              \
    {                                               \
        if( !( cond ) )                             \
        {                                           \
            printf( "FAIL %s:%d: %s\n",             \
                    __FILE__, __LINE__, ( msg ) );  \
            iFailures++;                            \
        }                                           \
    } while( 0 )

static void testIsAlarmValue( void )
{
    CHECK( iIsAlarmValue( 41, 40 ) == 1, "41 > 40 es alarma" );
    CHECK( iIsAlarmValue( 40, 40 ) == 0, "40 > 40 no es alarma (estricto)" );
    CHECK( iIsAlarmValue( 0, 40 ) == 0, "0 > 40 no es alarma" );
    CHECK( iIsAlarmValue( 86, 85 ) == 1, "86 > 85 es alarma" );
    CHECK( iIsAlarmValue( 85, 85 ) == 0, "85 > 85 no es alarma" );
}

static void testClampPercent( void )
{
    CHECK( uiClampPercent( -5, 50 ) == 0U, "negativo -> 0" );
    CHECK( uiClampPercent( 0, 50 ) == 0U, "0 -> 0" );
    CHECK( uiClampPercent( 25, 50 ) == 50U, "25/50 -> 50%" );
    CHECK( uiClampPercent( 50, 50 ) == 100U, "50/50 -> 100%" );
    CHECK( uiClampPercent( 999, 50 ) == 100U, "por encima del max -> 100%" );
    CHECK( uiClampPercent( 10, 0 ) == 0U, "maximo 0 -> 0 (sin division por cero)" );
}

static void testFormatBar( void )
{
    char pcBar[ 32 ];

    vFormatBar( pcBar, sizeof( pcBar ), 0U, 10U );
    CHECK( strcmp( pcBar, "----------" ) == 0, "0% -> todo guiones" );

    vFormatBar( pcBar, sizeof( pcBar ), 100U, 10U );
    CHECK( strcmp( pcBar, "##########" ) == 0, "100% -> todo almohadillas" );

    vFormatBar( pcBar, sizeof( pcBar ), 50U, 10U );
    CHECK( strcmp( pcBar, "#####-----" ) == 0, "50% -> mitad" );

    vFormatBar( pcBar, sizeof( pcBar ), 150U, 8U );
    CHECK( strcmp( pcBar, "########" ) == 0, "150% se recorta a 100%" );

    vFormatBar( pcBar, 4U, 100U, 24U );
    CHECK( strlen( pcBar ) == 3U, "el ancho se limita al buffer" );
}

static void testFormatSpark( void )
{
    char pcSpark[ 64 ];
    int aiEmpty[ 4 ] = { 0, 0, 0, 0 };
    int aiRamp[ 4 ] = { 15, 40, 15, 40 };

    vFormatSpark( pcSpark, sizeof( pcSpark ), aiEmpty, 0U, 4U, 0, 100 );
    CHECK( strcmp( pcSpark, "(sin datos)" ) == 0, "sin lecturas -> (sin datos)" );

    vFormatSpark( pcSpark, sizeof( pcSpark ), aiRamp, 4U, 4U, 15, 40 );
    CHECK( strlen( pcSpark ) == 4U, "4 lecturas -> 4 caracteres" );
    CHECK( pcSpark[ 0 ] == ' ', "valor minimo -> nivel 0" );
    CHECK( pcSpark[ 1 ] == '@', "valor maximo -> nivel 9" );
    CHECK( pcSpark[ 2 ] == ' ', "repite minimo" );
    CHECK( pcSpark[ 3 ] == '@', "repite maximo" );

    vFormatSpark( pcSpark, sizeof( pcSpark ), aiRamp, 2U, 4U, 15, 40 );
    CHECK( strlen( pcSpark ) == 2U, "aun sin llenar el historial" );
    CHECK( pcSpark[ 0 ] == ' ' && pcSpark[ 1 ] == '@', "respeta el orden de escritura" );

    {
        int aiMid[ 3 ] = { 15, 27, 40 };

        vFormatSpark( pcSpark, sizeof( pcSpark ), aiMid, 3U, 3U, 15, 40 );
        CHECK( pcSpark[ 0 ] == ' ', "15 = nivel 0" );
        CHECK( pcSpark[ 1 ] == '=', "27 = nivel ~5 (mitad)" );
        CHECK( pcSpark[ 2 ] == '@', "40 = nivel 9" );
    }

    {
        int aiRing[ 3 ] = { 10, 20, 30 };

        vFormatSpark( pcSpark, sizeof( pcSpark ), aiRing, 5U, 3U, 0, 100 );
        CHECK( strlen( pcSpark ) == 3U, "anillo: 3 valores visibles" );
        CHECK( pcSpark[ 0 ] == ':', "el mas antiguo (30) va primero" );
        CHECK( pcSpark[ 1 ] == ' ', "10 en medio" );
        CHECK( pcSpark[ 2 ] == '.', "el mas reciente (20) va al final" );
    }
}

static void testRandomRange( void )
{
    unsigned int uiSeedA = 1234U;
    unsigned int uiSeedB = 1234U;
    int iFirst;
    int iValue;
    int i;

    iFirst = iRandomRange( 15, 38, &uiSeedA );
    CHECK( ( iFirst >= 15 ) && ( iFirst <= 38 ), "dentro de [15,38]" );

    iValue = iRandomRange( 15, 38, &uiSeedB );
    CHECK( iValue == iFirst, "misma semilla -> mismo valor (determinista)" );

    for( i = 0; i < 200; i++ )
    {
        int iV = iRandomRange( 30, 82, &uiSeedA );

        if( ( iV < 30 ) || ( iV > 82 ) )
        {
            CHECK( 0, "fuera de rango en la serie" );
            break;
        }
    }
}

int main( void )
{
    testIsAlarmValue();
    testClampPercent();
    testFormatBar();
    testFormatSpark();
    testRandomRange();

    if( iFailures == 0 )
    {
        printf( "OK: todos los tests de logica pasaron\n" );
        return 0;
    }

    printf( "%d tests fallidos\n", iFailures );
    return 1;
}
