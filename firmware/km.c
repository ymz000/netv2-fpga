#include <generated/csr.h>
#include <stdio.h>
#include <stdlib.h>

#include <irq.h>

#include "km.h"

#define SOURCE 1
#define SINK 0

unsigned char read_hdcp(unsigned char addr) {
  i2c_snoop_edid_snoop_adr_write( addr );
  return( i2c_snoop_edid_snoop_dat_read() );
}

#define CHECK_KM 0x86a4df6560c88eLL  // panasonic + LG
//#define CHECK_KM 0x225d10cee24175LL  // appleTV + LG
#define CHECK 0

#define DEBUG 0

int link_redo = 0;

void hdcp_init(void) {
  // unmask the interrupts for HDCP
  unsigned int mask;
  mask = irq_getmask();
  mask |= 1 << HDCP_INTERRUPT;
  irq_setmask(mask);
  wprintf("interrupt mask (hdcp): %x\n", mask);
  
  hdcp_ev_enable_write(1);
  wprintf("hdcp_ev_enable_read: %d\n", hdcp_ev_enable_read());
  
  //  hdcp_Aksv_mode_write(0);
  hdcp_Aksv_manual_write(0); // clear to zero for default, as it's rising-edge triggered
  hdcp_Aksv_mode_write(1);  // select manual Aksv mode
}

void hdcp_isr(void) {
  unsigned int stat;

  hdcp_ev_pending_write(1);

  if( derive_km() ) {
    link_redo = 1;
  }
  hdcp_Aksv_manual_write(1);
  
  hdcp_ev_enable_write(1);
  
  printf( "Km: %08x %08x\n", (unsigned long) (hdcp_Km_read() >> 32), (unsigned long) hdcp_Km_read() );
  hdcp_Aksv_manual_write(0);  // clear for next use: this is rising edge-triggered
  
}

int derive_km(void) {
    unsigned int num;
    int i;
    
    unsigned long long source_ksv = 0LL;
    unsigned long long sink_ksv = 0LL;
    unsigned long long ksv_temp = 0LL;
    unsigned long long Km = 0LL;
    unsigned long long Kmp = 0LL;
    
    unsigned long long source_pkey[40];
    unsigned long long sink_pkey[40];

    hdcp_Km_valid_write(0);
    for( i = 0; i < 5; i++ ) {
      sink_ksv <<= 8;
      sink_ksv |= (read_hdcp(4 - i) & 0xff);
    }

    for( i = 0; i < 5; i++ ) {
      source_ksv <<= 8;
      source_ksv |= (read_hdcp(4 - i + 0x10) & 0xff);
    }

    compute_keys( (unsigned long) (source_ksv >> 32), (unsigned long) source_ksv, SOURCE, source_pkey );
    compute_keys( (unsigned long) (sink_ksv >> 32), (unsigned long) sink_ksv, SINK, sink_pkey );
#if DEBUG
    wprintf( "source public ksv (lsb): %08x\n", (unsigned long) source_ksv );
    wprintf( "source public ksv (msb): %08x\n", (unsigned long) (source_ksv >> 32) );
    wprintf( "sink public ksv (lsb): %08x\n", (unsigned long) sink_ksv );
    wprintf( "sink public ksv (msb): %08x\n", (unsigned long) (sink_ksv >> 32) );
#endif

    ksv_temp = source_ksv; // source Ksv
    num = 0;
    for( i = 0; i < 40; i++ ) {
      if( ksv_temp & 1LL ) {
	num++;
	Km += sink_pkey[i]; // used to select sink's keys
	Km &=  0xFFFFFFFFFFFFFFLL;
	//	Km %=  72057594037927936LL;
	//	wprintf( "Km 0x%08x", (unsigned long) (Km >> 32) );
	//	wprintf( "%08x\n", (unsigned long) Km );
      }
      ksv_temp >>= 1LL;
    }
    //    printf( "num 1's: %d\n", num );
    // km is the sink km

    ksv_temp = sink_ksv; // sink Ksv
    num = 0;
    for( i = 0; i < 40; i++ ) {
      if( ksv_temp & 1LL ) {
	num++;
	Kmp += source_pkey[i]; // used to select source's keys
	Kmp &= 0xFFFFFFFFFFFFFFLL;
	//	Kmp %=  72057594037927936LL;
	//	printf( "Kmp %014llx\n", Kmp );
      }
      ksv_temp >>= 1LL;
    }
    //    printf( "num 1's: %d\n", num );
    // Kmp is the source Km
  
    Km &= 0xFFFFFFFFFFFFFFLL;
    Kmp &= 0xFFFFFFFFFFFFFFLL;
  
    //    wprintf( "\n" );
#if DEBUG
    wprintf( "Km (lsb): %08x\n", (unsigned long) (Km & 0xFFFFFFFF) );
    wprintf( "Km (msb): %08x\n", (unsigned long) (Km >> 32) );
    wprintf( "Km'(lsb): %08x\n", (unsigned long) (Kmp & 0xFFFFFFFF) );
    wprintf( "Km'(msb): %08x\n", (unsigned long) (Kmp >> 32) );
#endif

    if( Km != Kmp ) {
      wprintf( "Km is not equal to Km', can't encrypt this stream.\n" );
      return 1;
    }

    if( Km == 0 ) {
      wprintf( "Km is zero. This probably means derive_km was fired spuriously on disconnect.\n" );
      wprintf( "Aborting without doing anything, since Km = 0 is never a correct condition\n" );
      return 1;
    } else {
#if DEBUG
      wprintf( "Committing Km\n" );
#endif
      // now commit Km to the fpga
      //      if( Km != CHECK_KM ) {
      //	wprintf( "*****Km doesn't match check value*****\n" );
	//	wprintf( "Writing check Km instead\n" );
	//	Km = CHECK_KM;
      //      }
      
      unsigned char foo;
      for( i = 6; i >= 0; i-- ) {
	// start with the LSB, which gets written to to the highest CSR address
	foo = (unsigned char)(Km & 0xFF);
#if DEBUG
	wprintf( "Writing to %02x to %08x\n", foo, CSR_HDCP_BASE + i * 4);
#endif
	MMPTR(CSR_HDCP_BASE + i * 4) = foo;
	Km >>= 8;
      }

#if DEBUG
      wprintf( "Confirm check Km as writ: (LSB) %08x\n", (unsigned long) hdcp_Km_read() );
      wprintf( "Confirm check Km as writ: (MSB) %08x\n", (unsigned long) (hdcp_Km_read() >> 32) );
#endif
      //      for( i = 0; i < 7; i++ ) {
      //	write_km( i, Km & 0xFF );
      //	Km >>= 8;
      //      }

#if DEBUG
      printf( "Flipping Km valid\n" );
#endif
      // indicate Km ready
      hdcp_Km_valid_write(1);
      //write_km( 7, 1 );
    }

#if 0    
    int last_event;
    while( !elapsed(&last_event, SYSTEM_CLOCK_FREQUENCY) ) {
      ;
    }
    while( !elapsed(&last_event, SYSTEM_CLOCK_FREQUENCY) ) {
      ;
    }
    wprintf( "Invoking HPD\n" );
    hdcp_hpd_ena_write(1);
    elapsed(&last_event, SYSTEM_CLOCK_FREQUENCY);

    while( !elapsed(&last_event, SYSTEM_CLOCK_FREQUENCY) ) {
      ;
    }
    while( !elapsed(&last_event, SYSTEM_CLOCK_FREQUENCY) ) {
      ;
    }
    wprintf( "Releasing HPD\n" );
    hdcp_hpd_ena_write(0);
#endif
    
    return 0;
}
