/*
 This file is part of program wsprd, a detector/demodulator/decoder
 for the Weak Signal Propagation Reporter (WSPR) mode.
 
 File name: wsprd_utils.c
 
 Copyright 2001-2015, Joe Taylor, K1JT
 
 Most of the code is based on work by Steven Franke, K9AN, which
 in turn was based on earlier work by K1JT.
 
 Copyright 2014-2015, Steven Franke, K9AN
 
 License: GNU GPL v3
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "wsprd_utils.h"

#ifndef int32_t
#define int32_t int
#endif

void unpack50( signed char *dat, int32_t *n1, int32_t *n2 )
{
    int32_t i,i4;
    
    i=dat[0];
    i4=i&255;
    *n1=i4<<20;
    
    i=dat[1];
    i4=i&255;
    *n1=*n1+(i4<<12);
    
    i=dat[2];
    i4=i&255;
    *n1=*n1+(i4<<4);
    
    i=dat[3];
    i4=i&255;
    *n1=*n1+((i4>>4)&15);
    *n2=(i4&15)<<18;
    
    i=dat[4];
    i4=i&255;
    *n2=*n2+(i4<<10);
    
    i=dat[5];
    i4=i&255;
    *n2=*n2+(i4<<2);
    
    i=dat[6];
    i4=i&255;
    *n2=*n2+((i4>>6)&3);
}

int unpackcall( int32_t ncall, char *call )
{
    char c[]={'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E',
        'F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T',
        'U','V','W','X','Y','Z',' '};
    int32_t n;
    int i;
    char tmp[7];

    n=ncall;
    strcpy(call,"......");
    if (n < 262177560 ) {
        i=n%27+10;
        tmp[5]=c[i];
        n=n/27;
        i=n%27+10;
        tmp[4]=c[i];
        n=n/27;
        i=n%27+10;
        tmp[3]=c[i];
        n=n/27;
        i=n%10;
        tmp[2]=c[i];
        n=n/10;
        i=n%36;
        tmp[1]=c[i];
        n=n/36;
        i=n;
        tmp[0]=c[i];
        tmp[6]='\0';
        // remove leading whitespace
        for(i=0; i<5; i++) {
            if( tmp[i] != c[36] )
                break;
        }
        sprintf(call,"%-6s",&tmp[i]);
        // remove trailing whitespace
        for(i=0; i<6; i++) {
            if( call[i] == c[36] ) {
                call[i]='\0';
            }
        }
    } else {
        return 0;
    }
    return 1;
}

int unpackgrid( int32_t ngrid, char *grid)
{
    char c[]={'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E',
        'F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T',
        'U','V','W','X','Y','Z',' '};
    int dlat, dlong;
    
    ngrid=ngrid>>7;
    if( ngrid < 32400 ) {
        dlat=(ngrid%180)-90;
        dlong=(ngrid/180)*2 - 180 + 2;
        if( dlong < -180 )
            dlong=dlong+360;
        if( dlong > 180 )
            dlong=dlong+360;
        int nlong = 60.0*(180.0-dlong)/5.0;
        int n1 = nlong/240;
        int n2 = (nlong - 240*n1)/24;
        grid[0] = c[10+n1];
        grid[2]=  c[n2];
        
        int nlat = 60.0*(dlat+90)/2.5;
        n1 = nlat/240;
        n2 = (nlat-240*n1)/24;
        grid[1]=c[10+n1];
        grid[3]=c[n2];
    } else {
        strcpy(grid,"XXXX");
        return 0;
    }
    return 1;
}

int unpackpfx( int32_t nprefix, char *call)
{
    char nc, pfx[4]={'\0'}, tmpcall[7];
    int i;
    int32_t n;
    
    strcpy(tmpcall,call);
    if( nprefix < 60000 ) {
        // add a prefix of 1 to 3 characters
        n=nprefix;
        for (i=2; i>=0; i--) {
            nc=n%37;
            if( (nc >= 0) & (nc <= 9) ) {
                pfx[i]=nc+48;
            }
            else if( (nc >= 10) & (nc <= 35) ) {
                pfx[i]=nc+55;
            }
            else {
                pfx[i]=' ';
            }
            n=n/37;
        }

        char * p = strrchr(pfx,' ');
        strcpy(call, p ? p + 1 : pfx);
        strncat(call,"/",1);
        strncat(call,tmpcall,strlen(tmpcall));
        
    } else {
        // add a suffix of 1 or 2 characters
        nc=nprefix-60000;
        if( (nc >= 0) & (nc <= 9) ) {
            pfx[0]=nc+48;
            strcpy(call,tmpcall);
            strncat(call,"/",1);
            strncat(call,pfx,1);
        }
        else if( (nc >= 10) & (nc <= 35) ) {
            pfx[0]=nc+55;
            strcpy(call,tmpcall);
            strncat(call,"/",1);
            strncat(call,pfx,1);
        }
        else if( (nc >= 36) & (nc <= 125) ) {
            pfx[0]=(nc-26)/10+48;
            pfx[1]=(nc-26)%10+48;
            strcpy(call,tmpcall);
            strncat(call,"/",1);
            strncat(call,pfx,2);
        }
        else {
            return 0;
        }
    }
    return 1;
}

void deinterleave(unsigned char *sym)
{
    unsigned char tmp[162];
    unsigned char p, i, j;
    
    p=0;
    i=0;
    while (p<162) {
        j=((i * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32;
        if (j < 162 ) {
            tmp[p]=sym[j];
            p=p+1;
        }
        i=i+1;
    }
    for (i=0; i<162; i++) {
        sym[i]=tmp[i];
    }
}

// used by qsort
int doublecomp(const void* elem1, const void* elem2)
{
    if(*(const double*)elem1 < *(const double*)elem2)
        return -1;
    return *(const double*)elem1 > *(const double*)elem2;
}

int floatcomp(const void* elem1, const void* elem2)
{
    if(*(const float*)elem1 < *(const float*)elem2)
        return -1;
    return *(const float*)elem1 > *(const float*)elem2;
}

int unpk_(signed char *message, char *hashtab, char *loctab, char *call_loc_pow, char *callsign)
{
    int n1,n2,n3,ndbm,ihash,nadd,noprint=0;
    char grid[5],grid6[7],cdbm[4];
    
    unpack50(message,&n1,&n2);
    if( !unpackcall(n1,callsign) ) return 1;
    if( !unpackgrid(n2, grid) ) return 1;
    int ntype = (n2&127) - 64;
    callsign[12]=0;
    grid[4]=0;

    /*
     Based on the value of ntype, decide whether this is a Type 1, 2, or
     3 message.
     
     * Type 1: 6 digit call, grid, power - ntype is positive and is a member
     of the set {0,3,7,10,13,17,20...60}
     
     * Type 2: extended callsign, power - ntype is positive but not
     a member of the set of allowed powers
     
     * Type 3: hash, 6 digit grid, power - ntype is negative.
     */

    if( (ntype >= 0) && (ntype <= 62) ) {
        int nu=ntype%10;
        if( nu == 0 || nu == 3 || nu == 7 ) {
            ndbm=ntype;
            memset(call_loc_pow,0,sizeof(char)*23);
            sprintf(cdbm,"%2d",ndbm);
            strncat(call_loc_pow,callsign,strlen(callsign));
            strncat(call_loc_pow," ",1);
            strncat(call_loc_pow,grid,4);
            strncat(call_loc_pow," ",1);
            strncat(call_loc_pow,cdbm,2);
            strncat(call_loc_pow,"\0",1);
            ihash=nhash(callsign,strlen(callsign),(uint32_t)146);
            strcpy(hashtab+ihash*13,callsign);
            strcpy(loctab+ihash*5,grid);
        } else {
            nadd=nu;
            if( nu > 3 ) nadd=nu-3;
            if( nu > 7 ) nadd=nu-7;
            n3=n2/128+32768*(nadd-1);
            if( !unpackpfx(n3,callsign) ) return 1;
            ndbm=ntype-nadd;
            memset(call_loc_pow,0,sizeof(char)*23);
            sprintf(cdbm,"%2d",ndbm);
            strncat(call_loc_pow,callsign,strlen(callsign));
            strncat(call_loc_pow," ",1);
            strncat(call_loc_pow,cdbm,2);
            strncat(call_loc_pow,"\0",1);
            int nu=ndbm%10;
            if( nu == 0 || nu == 3 || nu == 7 || nu == 10 ) { //make sure power is OK
                ihash=nhash(callsign,strlen(callsign),(uint32_t)146);
                strcpy(hashtab+ihash*13,callsign);
            } else noprint=1;
        }
    } else if ( ntype < 0 ) {
        ndbm=-(ntype+1);
        memset(grid6,0,sizeof(char)*7);
//        size_t len=strlen(callsign);
        size_t len=6;
        strncat(grid6,callsign+len-1,1);
        strncat(grid6,callsign,len-1);
        int nu=ndbm%10;
        if ((nu != 0 && nu != 3 && nu != 7 && nu != 10) ||
            !isalpha(grid6[0]) || !isalpha(grid6[1]) ||
            !isdigit(grid6[2]) || !isdigit(grid6[3])) {
               // not testing 4'th and 5'th chars because of this case: <PA0SKT/2> JO33 40
               // grid is only 4 chars even though this is a hashed callsign...
               //         isalpha(grid6[4]) && isalpha(grid6[5]) ) ) {
            noprint=1;
        } 
        
        ihash=(n2-ntype-64)/128;
        if( strncmp(hashtab+ihash*13,"\0",1) != 0 ) {
            sprintf(callsign,"<%s>",hashtab+ihash*13);
        } else {
            sprintf(callsign,"%5s","<...>");
        }
        
        memset(call_loc_pow,0,sizeof(char)*23);
        sprintf(cdbm,"%2d",ndbm);
        strncat(call_loc_pow,callsign,strlen(callsign));
        strncat(call_loc_pow," ",1);
        strncat(call_loc_pow,grid6,strlen(grid6));
        strncat(call_loc_pow," ",1);
        strncat(call_loc_pow,cdbm,2);
        strncat(call_loc_pow,"\0",1);
        
        
        // I don't know what to do with these... They show up as "A000AA" grids.
        if( ntype == -64 ) noprint=1;
    }
    return noprint;
}
