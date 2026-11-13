#!/bin/sh
# Checks the C translations of WSJT-X's Fortran against the Fortran itself:
#   wsprd/osdwspr.c  vs lib/wsprd/osdwspr.f90 + lib/indexx.f90
#   grayline.c       vs lib/grayline.f90 + lib/sun.f90 + lib/grid2deg.f90
# on the same inputs, requiring identical output (the OSD distance compared
# bit for bit). Needs gcc, gfortran and a WSJT-X source tree:
#
#   ./test_osd.sh /path/to/wsjtx        (3.0.2 was used)
#
# Part of AetherSDR, GPL-3.0-or-later.
set -eu
WSJTX=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"

# OSD: 2000 received words at each depth 1-5.
cat > gen.c <<'EOF'
#include <stdio.h>
#include <stdint.h>
int main(void) {
    uint32_t s = 12345;
    FILE *f = fopen("words.bin", "wb");
    for (int w = 0; w < 2000; w++) {
        float v[162];
        for (int i = 0; i < 162; i++) {
            s = s * 1664525u + 1013904223u;
            int m = (int)((s >> 8) % 128), sign = ((s >> 20) & 7) ? 1 : -1, bias = (w % 3 == 0) ? 40 : 0;
            v[i] = (float)(sign * m + (sign > 0 ? bias : -bias));
            if (v[i] > 127) v[i] = 127;
            if (v[i] < -128) v[i] = -128;
        }
        fwrite(v, sizeof v, 1, f);
    }
    return fclose(f);
}
EOF
cat > osd.f90 <<'EOF'
program drv
  real ss(162), dmin
  integer*1 apmask(162), cw(162)
  integer nh, nd, w, i, nd0
  character(len=162) s
  apmask=0
  open(10,file='words.bin',access='stream',form='unformatted')
  do nd0=1,5
    rewind(10)
    do w=1,2000
      read(10) ss
      nd=nd0
      call osdwspr(ss,apmask,nd,cw,nh,dmin)
      do i=1,162
        s(i:i)=achar(48+cw(i))
      enddo
      write(*,'(i1,1x,a,1x,i4,1x,z8.8)') nd0, s, nh, transfer(dmin,1)
    enddo
  enddo
end program
EOF
cat > osd.c <<'EOF'
#include <stdio.h>
#include <string.h>
void osdwspr_(float[], unsigned char[], int *, unsigned char[], int *, float *);
int main(void) {
    FILE *f = fopen("words.bin", "rb");
    float ss[162], dmin;
    unsigned char ap[162] = {0}, cw[162];
    int nh;
    for (int nd0 = 1; nd0 <= 5; nd0++) {
        rewind(f);
        for (int w = 0; w < 2000; w++) {
            if (fread(ss, sizeof ss, 1, f) != 1) return 1;
            int nd = nd0;
            osdwspr_(ss, ap, &nd, cw, &nh, &dmin);
            char s[163];
            for (int i = 0; i < 162; i++) s[i] = (char)('0' + cw[i]);
            s[162] = 0;
            unsigned u;
            memcpy(&u, &dmin, 4);
            printf("%d %s %4d %08X\n", nd0, s, nh, u);
        }
    }
    return 0;
}
EOF
gcc -O2 -o gen gen.c && ./gen
gfortran -O2 -w -o osd-f osd.f90 "$WSJTX/lib/wsprd/osdwspr.f90" "$WSJTX/lib/indexx.f90"
gcc -O2 -o osd-c osd.c "$HERE/wsprd/osdwspr.c" -lm
./osd-f > osd-f.out
./osd-c > osd-c.out
cmp osd-f.out osd-c.out
echo "OSD: $(wc -l < osd-f.out) results identical"

# Grayline: 8 grids, 3 gray-line widths, every 4 minutes on 36 days of 2026.
cat > gray.f90 <<'EOF'
program drv
  character*6 grids(8)
  integer im, id, k, ig, isun, idur, durs(3), nmin
  real uth
  data grids/'DN18pg','FN42  ','JO31  ','PM95  ','QF56  ','KP20  ','RE78ir','AA00  '/
  data durs/60,30,120/
  do ig=1,8
   do k=1,3
    idur=durs(k)
    do im=1,12
     do id=1,28,9
      do nmin=0,1439,4
        uth=nmin/60.0 + 0.0333
        call grayline(2026,im,id,uth,grids(ig),idur,isun)
        write(*,'(i1,1x,i3,1x,i2,1x,i2,1x,i4,1x,i1)') ig, idur, im, id, nmin, isun
      enddo
     enddo
    enddo
   enddo
  enddo
end program
EOF
cat > gray.c <<'EOF'
#include <stdio.h>
#include "grayline.h"
int main(void) {
    const char *grids[8] = {"DN18pg", "FN42", "JO31", "PM95", "QF56", "KP20", "RE78ir", "AA00"};
    int durs[3] = {60, 30, 120};
    for (int ig = 0; ig < 8; ig++)
        for (int k = 0; k < 3; k++)
            for (int im = 1; im <= 12; im++)
                for (int id = 1; id <= 28; id += 9)
                    for (int nmin = 0; nmin <= 1439; nmin += 4) {
                        float uth = (float)nmin / 60.0f + 0.0333f;
                        printf("%d %3d %2d %2d %4d %d\n", ig + 1, durs[k], im, id, nmin,
                               wspr_grayline(2026, im, id, uth, grids[ig], durs[k]));
                    }
    return 0;
}
EOF
gfortran -O2 -w -o gray-f gray.f90 "$WSJTX/lib/grayline.f90" "$WSJTX/lib/sun.f90" "$WSJTX/lib/grid2deg.f90"
gcc -O2 -I"$HERE" -o gray-c gray.c "$HERE/grayline.c" -lm
./gray-f > gray-f.out
./gray-c > gray-c.out
cmp gray-f.out gray-c.out
echo "Grayline: $(wc -l < gray-f.out) periods identical"
