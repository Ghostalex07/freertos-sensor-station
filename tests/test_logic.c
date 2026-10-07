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
    CHECK( iIsAlarmValue( 41, 40 ) == 1, "41 > 40 is an alarm" );
    CHECK( iIsAlarmValue( 40, 40 ) == 0, "40 > 40 is not an alarm (strict)" );
    CHECK( iIsAlarmValue( 0, 40 ) == 0, "0 > 40 is not an alarm" );
    CHECK( iIsAlarmValue( 86, 85 ) == 1, "86 > 85 is an alarm" );
    CHECK( iIsAlarmValue( 85, 85 ) == 0, "85 > 85 is not an alarm" );
}

static void testClampPercent( void )
{
    CHECK( uiClampPercent( -5, 50 ) == 0U, "negative -> 0" );
    CHECK( uiClampPercent( 0, 50 ) == 0U, "0 -> 0" );
    CHECK( uiClampPercent( 25, 50 ) == 50U, "25/50 -> 50%" );
    CHECK( uiClampPercent( 50, 50 ) == 100U, "50/50 -> 100%" );
    CHECK( uiClampPercent( 999, 50 ) == 100U, "above the max -> 100%" );
    CHECK( uiClampPercent( 10, 0 ) == 0U, "max 0 -> 0 (no division by zero)" );
}

static void testFormatBar( void )
{
    char pcBar[ 32 ];

    vFormatBar( pcBar, sizeof( pcBar ), 0U, 10U );
    CHECK( strcmp( pcBar, "----------" ) == 0, "0% -> all dashes" );

    vFormatBar( pcBar, sizeof( pcBar ), 100U, 10U );
    CHECK( strcmp( pcBar, "##########" ) == 0, "100% -> all hashes" );

    vFormatBar( pcBar, sizeof( pcBar ), 50U, 10U );
    CHECK( strcmp( pcBar, "#####-----" ) == 0, "50% -> half" );

    vFormatBar( pcBar, sizeof( pcBar ), 150U, 8U );
    CHECK( strcmp( pcBar, "########" ) == 0, "150% is clamped to 100%" );

    vFormatBar( pcBar, 4U, 100U, 24U );
    CHECK( strlen( pcBar ) == 3U, "width is limited by the buffer" );
}

static void testFormatSpark( void )
{
    char pcSpark[ 64 ];
    int aiEmpty[ 4 ] = { 0, 0, 0, 0 };
    int aiRamp[ 4 ] = { 15, 40, 15, 40 };

    vFormatSpark( pcSpark, sizeof( pcSpark ), aiEmpty, 0U, 4U, 0, 100 );
    CHECK( strcmp( pcSpark, "(no data)" ) == 0, "no readings -> (no data)" );

    vFormatSpark( pcSpark, sizeof( pcSpark ), aiRamp, 4U, 4U, 15, 40 );
    CHECK( strlen( pcSpark ) == 4U, "4 readings -> 4 characters" );
    CHECK( pcSpark[ 0 ] == ' ', "minimum value -> level 0" );
    CHECK( pcSpark[ 1 ] == '@', "maximum value -> level 9" );
    CHECK( pcSpark[ 2 ] == ' ', "repeats the minimum" );
    CHECK( pcSpark[ 3 ] == '@', "repeats the maximum" );

    vFormatSpark( pcSpark, sizeof( pcSpark ), aiRamp, 2U, 4U, 15, 40 );
    CHECK( strlen( pcSpark ) == 2U, "history not filled yet" );
    CHECK( pcSpark[ 0 ] == ' ' && pcSpark[ 1 ] == '@', "respects the write order" );

    {
        int aiMid[ 3 ] = { 15, 27, 40 };

        vFormatSpark( pcSpark, sizeof( pcSpark ), aiMid, 3U, 3U, 15, 40 );
        CHECK( pcSpark[ 0 ] == ' ', "15 = level 0" );
        CHECK( pcSpark[ 1 ] == '=', "27 = level ~5 (half)" );
        CHECK( pcSpark[ 2 ] == '@', "40 = level 9" );
    }

    {
        int aiRing[ 3 ] = { 10, 20, 30 };

        vFormatSpark( pcSpark, sizeof( pcSpark ), aiRing, 5U, 3U, 0, 100 );
        CHECK( strlen( pcSpark ) == 3U, "ring: 3 visible values" );
        CHECK( pcSpark[ 0 ] == ':', "the oldest (30) comes first" );
        CHECK( pcSpark[ 1 ] == ' ', "10 in the middle" );
        CHECK( pcSpark[ 2 ] == '.', "the newest (20) goes last" );
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
    CHECK( ( iFirst >= 15 ) && ( iFirst <= 38 ), "inside [15,38]" );

    iValue = iRandomRange( 15, 38, &uiSeedB );
    CHECK( iValue == iFirst, "same seed -> same value (deterministic)" );

    for( i = 0; i < 200; i++ )
    {
        int iV = iRandomRange( 30, 82, &uiSeedA );

        if( ( iV < 30 ) || ( iV > 82 ) )
        {
            CHECK( 0, "out of range in the series" );
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
        printf( "OK: all logic tests passed\n" );
        return 0;
    }

    printf( "%d failing tests\n", iFailures );
    return 1;
}
