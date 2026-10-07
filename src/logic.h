#ifndef LOGIC_H
#define LOGIC_H

#include <stddef.h>

void vFormatSpark( char * pcBuffer,
                   size_t xBufferSize,
                   const int * piHistory,
                   unsigned int uiNext,
                   unsigned int uiHistoryLength,
                   int iMinimum,
                   int iMaximum );

void vFormatBar( char * pcBuffer,
                 size_t xBufferSize,
                 unsigned int uiPercent,
                 unsigned int uiWidth );

unsigned int uiClampPercent( int iValue,
                             int iMaximum );

int iIsAlarmValue( int iValue,
                   int iThreshold );

int iRandomRange( int iMin,
                  int iMax,
                  unsigned int * puiSeed );

#endif
