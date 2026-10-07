#include "logic.h"

#include <stdio.h>
#include <stdlib.h>

void vFormatSpark( char * pcBuffer,
                   size_t xBufferSize,
                   const int * piHistory,
                   unsigned int uiNext,
                   unsigned int uiHistoryLength,
                   int iMinimum,
                   int iMaximum )
{
    static const char pcLevels[] = " .:-=+*#%@";
    unsigned int uiCount;
    unsigned int uiStart;
    unsigned int uiIndex;

    if( uiHistoryLength == 0U )
    {
        uiHistoryLength = 1U;
    }

    if( uiNext < uiHistoryLength )
    {
        uiCount = uiNext;
        uiStart = 0U;
    }
    else
    {
        uiCount = uiHistoryLength;
        uiStart = uiNext - uiHistoryLength;
    }

    if( ( uiCount == 0U ) || ( xBufferSize < 2U ) )
    {
        ( void ) snprintf( pcBuffer, xBufferSize, "(no data)" );
        return;
    }

    if( uiCount > ( xBufferSize - 1U ) )
    {
        uiCount = ( unsigned int ) ( xBufferSize - 1U );
    }

    for( uiIndex = 0U; uiIndex < uiCount; uiIndex++ )
    {
        int iValue = piHistory[ ( uiStart + uiIndex ) % uiHistoryLength ];
        int iLevel;

        if( iValue <= iMinimum )
        {
            iLevel = 0;
        }
        else if( iValue >= iMaximum )
        {
            iLevel = 9;
        }
        else
        {
            iLevel = ( ( iValue - iMinimum ) * 9 ) / ( iMaximum - iMinimum );
        }

        pcBuffer[ uiIndex ] = pcLevels[ iLevel ];
    }

    pcBuffer[ uiIndex ] = '\0';
}

void vFormatBar( char * pcBuffer,
                 size_t xBufferSize,
                 unsigned int uiPercent,
                 unsigned int uiWidth )
{
    unsigned int uiFilled;
    unsigned int uiIndex;

    if( uiPercent > 100U )
    {
        uiPercent = 100U;
    }

    if( uiWidth >= xBufferSize )
    {
        uiWidth = ( unsigned int ) ( xBufferSize - 1U );
    }

    uiFilled = ( uiPercent * uiWidth ) / 100U;

    for( uiIndex = 0U; uiIndex < uiFilled; uiIndex++ )
    {
        pcBuffer[ uiIndex ] = '#';
    }

    for( ; uiIndex < uiWidth; uiIndex++ )
    {
        pcBuffer[ uiIndex ] = '-';
    }

    pcBuffer[ uiIndex ] = '\0';
}

unsigned int uiClampPercent( int iValue,
                             int iMaximum )
{
    unsigned int uiPercent;

    if( iMaximum <= 0 )
    {
        return 0U;
    }

    if( iValue <= 0 )
    {
        return 0U;
    }

    if( iValue >= iMaximum )
    {
        return 100U;
    }

    uiPercent = ( unsigned int ) ( ( iValue * 100 ) / iMaximum );

    return uiPercent;
}

int iIsAlarmValue( int iValue,
                   int iThreshold )
{
    return ( iValue > iThreshold ) ? 1 : 0;
}

int iRandomRange( int iMin,
                  int iMax,
                  unsigned int * puiSeed )
{
    return iMin + ( int ) ( rand_r( puiSeed ) % ( unsigned int ) ( iMax - iMin + 1 ) );
}
