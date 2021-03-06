/*
 * rtl_fsk, turns your Realtek RTL2832 based DVB dongle into a FSK receiver
 * 
 * Based on rtl_sdr
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */



//added M flag and R flag (for Rs) as command line options 

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <getopt.h>


#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "getopt/getopt.h"
#endif

#include "rtl-sdr.h"
#include "convenience/convenience.h"
#include "convenience/rtl_convenience.h"
#include "fsk.h"
#include "libcsdr.h"
#include "ldpc_codes.h"
#include "freedv_api.h"

/* rtlsdr  ------------------------------------*/

#define DEFAULT_SAMPLE_RATE		1800000
#define DEFAULT_BANDWIDTH		0	      /* automatic bandwidth */
#define DEFAULT_BUF_LENGTH		(16 * 16384)
#define MINIMAL_BUF_LENGTH		512
#define MAXIMAL_BUF_LENGTH		(256 * 16384)
#define BUF_SZ                          8192

/* fsk modem ----------------------------------*/

#define CSDR_BUFSIZE 1024
#define DEFAULT_MODEM_SAMPLE_RATE       40000        /* sample rate we run demod at */
#define DEFAULT_SYMBOL_RATE		10000        /* symbols/s */
#define DEFAULT_M		        2            /* 2FSK      */
#define NDFT                            256          /* number of DFT points on dashboard */
#define DEFAULT_CHANNEL_WIDTH           25000        /* 25 kHz channel for freq est */
#define NORM_RX_TIMING_LOG_SZ           1024

static int do_exit = 0;
static uint32_t bytes_to_read = 0;
static rtlsdr_dev_t *dev = NULL;
static struct FSK *fsk;
static uint32_t out_block_size = DEFAULT_BUF_LENGTH;
static unsigned char *rawbuf;
static size_t nrawbuf = 0;
static size_t nrawbuf_max = 0;
static int dashboard = 0;
static int sockfd;
static struct sockaddr_in serveraddr;
static int portno = 8001;

static uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
static uint32_t modem_samp_rate = DEFAULT_MODEM_SAMPLE_RATE;
static uint32_t sample_counter;
static float norm_rx_timing_log[NORM_RX_TIMING_LOG_SZ];
static uint32_t norm_rx_timing_log_index = 0;
static int fsk_lower = 0;
static int fsk_upper = 0;
static int output_bits;

static int csdr_factor;
static float csdr_in[CSDR_BUFSIZE*2];
static float csdr_out[CSDR_BUFSIZE*2];
static int csdr_nout, csdr_nin;
static int csdr_padded_taps_length;
static float *csdr_taps;

static COMP* modembuf;
static size_t nmodembuf = 0;
static size_t nmodembuf_max = 0;

static struct freedv *freedv = NULL;
static int fsk_ldpc = 0;
static uint8_t *bytes_out;
static uint8_t *zeros_out;
static int verbose = 0;
static int data_bytes_per_frame;
static int periodic_output = 0;
static uint8_t filter = 0;
static int inject_snr = 0;
static int log_snr = 0;
static float RxGain = 0.0;
static int Rs = DEFAULT_SYMBOL_RATE;

void usage(void)
{
	fprintf(stderr,
		"rtl_fsk, a FSK demodulator for RTL2832 based DVB-T receivers\n\n"
		"Usage:\t -f frequency_to_tune_to [Hz]\n"
		"\t[-a FSK modem sample rate (default: %d Hz)]\n"
		"\t[-s samplerate (default: %d Hz)]\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-e extended gain -e 0xlmi (l,m,i single hex digits 0-f)\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-M modn order (default: 2)]\n"
		"\t[-n number of samples to read (default: 0, infinite)]\n"
		"\t[-q periodic | status byte | data bytes | output (default: off)]\n"
		"\t[-r FSK modem symbol rate (default: %d Hz)]\n"
		"\t[-m number of FSK modem tones (default: %d)]\n"
		"\t[-u hostname (optional hostname:8001 where we send UDP dashboard diagnostics)\n"
                "\t[-x output complex float samples (default output demodulated bits)]\n"
                "\t[-t toneSpacingHz use 'mask' freq est]\n"
                "\t[--code CodeName    Use LDPC code CodeName] (note packed bytes out)\n"
                "\t[--listcodes        List available LDPC codes]\n"
                "\t[--testframes]      Built in testframe mode\n"
                "\t[--filter Byte      Filter (don't output) FSK_LDPC frames starting with (non-zero) Byte\n"
                "\t[--injectsnr        inject S and N floats into packet output\n"
                "\t[--logsnr           SNR info on stderr when packet received\n"
		"\tfilename ( '-' dumps bits to stdout)\n\n", DEFAULT_MODEM_SAMPLE_RATE, DEFAULT_SAMPLE_RATE, DEFAULT_SYMBOL_RATE, DEFAULT_M);
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
	rtlsdr_cancel_async(dev);
}
#endif


/* CSDR decimation helper functions -------------------------------------------------*/

static void csdr_decimate_cc(float *out, float *in, int the_bufsize, float *taps, int taps_length, int factor, int *nout, int *next_nin) {
    int output_size, input_skip;
    
    output_size=fir_decimate_cc((complexf*)in, (complexf*)out, the_bufsize, factor, taps, taps_length);
    input_skip=factor*output_size;
    memmove((complexf*)in,((complexf*)in)+input_skip,(the_bufsize-input_skip)*sizeof(complexf));
    *nout = output_size;
    *next_nin = input_skip;
}

static float *csdr_init_decimate_cc(int factor, float transition_bw, window_t window, int *padded_taps_length) {
    int    taps_length;
    float *taps;
    int    i;
    
    fprintf(stderr,"fir_decimate_cc: factor = %d window = %s\n", factor, firdes_get_string_from_window(window));
    taps_length = firdes_filter_len(transition_bw);
    fprintf(stderr,"fir_decimate_cc: taps_length = %d\n",taps_length);

    // allocate taps array
    *padded_taps_length = taps_length;
#define NEON_ALIGNMENT (4*4*2)
#ifdef NEON_OPTS
    errhead(); fprintf(stderr,"taps_length = %d\n", taps_length);
    *padded_taps_length = taps_length+(NEON_ALIGNMENT/4)-1 - ((taps_length+(NEON_ALIGNMENT/4)-1)%(NEON_ALIGNMENT/4));
    fprintf(stderr,"padded_taps_length = %d\n", *padded_taps_length);

    taps = (float*) (float*)malloc((*padded_taps_length+NEON_ALIGNMENT)*sizeof(float));
    fprintf(stderr,"taps = %x\n", taps);
    taps =  (float*)((((unsigned)taps)+NEON_ALIGNMENT-1) & ~(NEON_ALIGNMENT-1));
    errhead(); fprintf(stderr,"NEON aligned taps = %x\n", taps);
    for(i=0;i<*padded_taps_length-taps_length;i++) taps[taps_length+i]=0;
#else
    taps = (float*)malloc(taps_length*sizeof(float));
#endif

    firdes_lowpass_f(taps,taps_length, 0.5/(float)factor,window);
    return taps;
}


/* dashboard functions ------------------------------------------*/

static void udp_sendbuf(char buf[]) {
    int n;

    /* send the message to the server */
    n = sendto(sockfd, buf, strlen(buf), 0, (const struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (n < 0)  {
        fprintf(stderr, "ERROR in sendto\n");
        exit(1);
    }
}
                                
void update_dashboard(struct FSK *fsk) {
    unsigned int i;
    
    /* update buffer of timing samples */
    if (norm_rx_timing_log_index < NORM_RX_TIMING_LOG_SZ)
        norm_rx_timing_log[norm_rx_timing_log_index++] = fsk->norm_rx_timing;
    else
        fprintf(stderr, "norm_rx_timing_log full!");
    sample_counter += fsk_nin(fsk);

    if (sample_counter > modem_samp_rate) {
        /* one second has passed, lets send some dashboard information */
        char   buf[BUF_SZ];
        char   buf1[BUF_SZ];
        size_t Ndft;
        float *f_est, sum;
        int    m, step, start, k;
        float  SfdB[NDFT];
                            
        sample_counter -= modem_samp_rate;
        buf[0]=0;

        /* Current magnitude spectrum Sf[] from freq estimator */

        /* Limit spectrum to NDFT points */
        if (fsk->Ndft > NDFT) {
            step = fsk->Ndft/NDFT;
            Ndft = NDFT;
        } else {
            step = 1;
            Ndft = fsk->Ndft;
        }                                  
                                
        for(i=0, start=0; i<Ndft; i++, start+=step) {
            /* Sf[] array is linear magnitudes */
            sum = 0.0;
            for(k=0; k<step; k++)
                sum += fsk->Sf[start+k];
            SfdB[i] = 20.0*log10(sum+1E-6);
        }
                                    
        snprintf(buf1, BUF_SZ, "{"); strncat(buf, buf1, BUF_SZ-1);
        snprintf(buf1, BUF_SZ, "\"SfdB\":["); strncat(buf, buf1, BUF_SZ);
        for(i=0; i<Ndft; i++) {
            snprintf(buf1, BUF_SZ, "%f",SfdB[i]); strncat(buf, buf1, BUF_SZ);
            if(i<Ndft-1) { snprintf(buf1, BUF_SZ, ", "); strncat(buf, buf1, BUF_SZ); }
        }
        snprintf(buf1, BUF_SZ, "]"); strncat(buf, buf1, BUF_SZ);                           
                            
        /* FSK tone freq estimates */

        snprintf(buf1, BUF_SZ, ", \"fsk_lower_Hz\":%d, \"fsk_upper_Hz\":%d",
                 fsk_lower, fsk_upper); strncat(buf, buf1, BUF_SZ);
        if (fsk->freq_est_type)
            f_est = fsk->f2_est;
        else
            f_est = fsk->f_est;
        snprintf(buf1, BUF_SZ, ", \"f_est_Hz\":["); strncat(buf, buf1, BUF_SZ);
        for(m=0; m<fsk->mode; m++) {
            snprintf(buf1, BUF_SZ, "%f", f_est[m]); strncat(buf, buf1, BUF_SZ);
            if (m < (fsk->mode-1)) { snprintf(buf1, BUF_SZ, ", "); strncat(buf, buf1, BUF_SZ); }
        }
        snprintf(buf1, BUF_SZ, "]"); strncat(buf, buf1, BUF_SZ);

        snprintf(buf1, BUF_SZ, ", \"norm_rx_timing\":["); strncat(buf, buf1, BUF_SZ);
        for(i=0; i<norm_rx_timing_log_index; i++) {
            snprintf(buf1, BUF_SZ, "%f", norm_rx_timing_log[i]); strncat(buf, buf1, BUF_SZ);
            if (i < (norm_rx_timing_log_index-1)) { snprintf(buf1, BUF_SZ, ", "); strncat(buf, buf1, BUF_SZ); }
        }
        snprintf(buf1, BUF_SZ, "]"); strncat(buf, buf1, BUF_SZ);
        norm_rx_timing_log_index = 0;
                            
        snprintf(buf1, BUF_SZ, ", \"SNRest_lin\":%f, \"Fs_Hz\":%d",
                 fsk->SNRest, fsk->Fs); strncat(buf, buf1, BUF_SZ);
                            
        /* finish up JSON and send to dashboard GUI over UDP */
        snprintf(buf1, BUF_SZ, "}\n"); strncat(buf, buf1, BUF_SZ); 
        udp_sendbuf(buf);
        if (verbose == 0) fprintf(stderr, ".");
    }
}

/* rtl_sdr functions ------------------------------------------*/

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
        unsigned char *pout;
        unsigned int   i;
        unsigned char  bitbuf[fsk->Nbits];
        COMP          *pmodembuf;
        int            prev_fsk_nin;
        int            nbytes = 0;
        uint8_t        rx_status = 0;
        
	if (ctx) {
		if (do_exit)
			return;

		if ((bytes_to_read > 0) && (bytes_to_read < len)) {
			len = bytes_to_read;
			do_exit = 1;
			rtlsdr_cancel_async(dev);
		}

                /*
		if (fwrite(buf, 1, len, (FILE*)ctx) != len) {
			fprintf(stderr, "Short write, samples lost, exiting!\n");
			rtlsdr_cancel_async(dev);
		}
                */

                /* resample rawbuf to demod sample rate ------------------------------- */

                memcpy(&rawbuf[nrawbuf],buf,len);
                nrawbuf += len;
                assert(nrawbuf < 2*out_block_size);
                assert(nrawbuf < nrawbuf_max);
                pout = rawbuf;
                pmodembuf = modembuf + nmodembuf;
                while(nrawbuf >= 2*(size_t)csdr_nin) {
                    complexf *pcsdr_in = ((complexf*)csdr_in)+(CSDR_BUFSIZE-csdr_nin);
                    for(i=0;i<(uint32_t)csdr_nin;i++) {
                        pcsdr_in[i].i = ((float)pout[2*i]-127.0)/128.0;
                        pcsdr_in[i].q = ((float)pout[2*i+1]-127.0)/128.0;
                    }
                    csdr_decimate_cc((float*)pmodembuf, csdr_in, CSDR_BUFSIZE, csdr_taps, csdr_padded_taps_length, csdr_factor,
                                     &csdr_nout, &csdr_nin);
                    /* place resampled signal in modem input buf */
                    pmodembuf += csdr_nout;
                    nmodembuf += csdr_nout;
                    assert(pmodem_buf < (modembuf + nmodembuf_max));
                    
                    pout += 2*csdr_nin;
                    nrawbuf -= 2*csdr_nin;
                    assert(nrawbuf >= 0);
                }
                /* copy left over rawbuf samples to start for next time */
                memmove(rawbuf,pout,nrawbuf);
              
                /* when we have fsk_nin() samples run demod ----------------------------- */
                
                pmodembuf = modembuf;
                while(nmodembuf >= fsk_nin(fsk)) {
                    prev_fsk_nin = fsk_nin(fsk);       /* fsk_nin gets updated in fsk_demod() */
                                                       /* note: in coded mode fsk_nin() == freedv_nin() */
                    if (output_bits == 0)
                        fwrite((float*)pmodembuf, sizeof(complexf), prev_fsk_nin, (FILE*)ctx); 
                    else {
                        if (fsk_ldpc == 0)
                            fsk_demod(fsk, bitbuf, pmodembuf);
                        else {
                            nbytes = freedv_rawdatacomprx(freedv, bytes_out, pmodembuf);
                            rx_status = freedv_get_rx_status(freedv);
                            /* optional filtering of packets */
                            if ((rx_status & FREEDV_RX_BITS) && filter) {
                                //fprintf(stderr,"bytes_out[0]: 0x%02x filter: 0x%02x\n", bytes_out[0], filter);
                                if (bytes_out[0] == filter)
                                    rx_status &= ~FREEDV_RX_BITS;
                            }
                            /* optional received SNR info in packet bytes for retransmission */
                            if (inject_snr && (rx_status & FREEDV_RX_BITS)) {
                                float S,N;
                                freedv_get_fsk_S_and_N(freedv, &S, &N);
                                //fprintf(stderr, "S: %f N: %f snr: %f\n", S, N, 10*log10(S/N));
                                memcpy(&bytes_out[2], (uint8_t*)&S, sizeof(float));
                                memcpy(&bytes_out[6], (uint8_t*)&N, sizeof(float));
                            }
                            /* optional SNR info to stderr for logging */
                            if (log_snr && (rx_status & FREEDV_RX_BITS)) {
                                float S,N,RxleveldBm, NodBmHz;
                                freedv_get_fsk_S_and_N(freedv, &S, &N);
                                RxleveldBm = 10*log10(S) + RxGain;
                                NodBmHz = 10*log10(N) + RxGain - 10*log10((float)Rs);;
                                fprintf(stderr, "%lu ", (unsigned long)time(NULL));
                                fprintf(stderr, "Rx Frame Rxloc: %6.2f Noloc: %6.2f snrloc: %5.2f ", RxleveldBm, NodBmHz, 10*log10(S/N));
                                memcpy(&S, &bytes_out[2], sizeof(float));                                
                                memcpy(&N, &bytes_out[6], sizeof(float));
                                if ((S>0) && (N>0)) {
                                    RxleveldBm = 10*log10(S) + RxGain;
                                    NodBmHz = 10*log10(N) + RxGain - 10*log10((float)Rs);;
                                    fprintf(stderr, "Rxrem: %6.2f Norem: %6.2f snrrem: %5.2f\n", RxleveldBm, NodBmHz, 10*log10(S/N));
                                }
                                else
                                    fprintf(stderr, "Srem: %6.2f Nrem: %6.2f snrrem: %5.2f\n", 0., 0., 0.);
                                   
                            }
                        }
                            
                    }
                    pmodembuf += prev_fsk_nin;
                    nmodembuf -= prev_fsk_nin;
                    assert(nmodembuf >= 0);

                    /* output demodulated bits - bunch of different modes */
                    if (output_bits) {
                        if (fsk_ldpc == 0)
                            /* vanilla uncoded */
                            fwrite(bitbuf, 1, fsk->Nbits, (FILE*)ctx);  /* one bit per byte */
                        else {
                            /* LDPC_FEC */
                            if (periodic_output) {
                                /* in this mode we output status (and data if available) regularly, which is useful
                                   for driving state machines */
                                fwrite(&rx_status, 1, 1, (FILE*)ctx);
                                /* write a dummy frame if no bytes available */
                                if (rx_status & FREEDV_RX_BITS) {
                                    fwrite(bytes_out, 1, nbytes, (FILE*)ctx);
                                }
                                else
                                    fwrite(zeros_out, 1, data_bytes_per_frame, (FILE*)ctx);
                                   
                            }
                            else /* in this mode we only output bytes when available */
                                if (rx_status & FREEDV_RX_BITS)
                                    fwrite(bytes_out, 1, nbytes, (FILE*)ctx);   /* packed bytes     */
                               
                        }
                    }
                    
                    if((FILE*)ctx == stdout) fflush((FILE*)ctx);
                    
                    if (dashboard) {
                        update_dashboard(fsk);
                    }
                }
                /* copy left over modem samples to start of buffer */
                memmove(modembuf, pmodembuf, nmodembuf*sizeof(COMP));
                
		if (bytes_to_read > 0)
			bytes_to_read -= len;
	}
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *filename = NULL;
	int n_read;
	int r, opt;
	int gain = 0;
	int ppm_error = 0;
	int sync_mode = 0;
	FILE *file;
	uint8_t *buffer;
	int dev_index = 0;
	int dev_given = 0;
        char hostname[256];
	uint32_t frequency = 100000000;
        int M = DEFAULT_M;
        int channel_width = DEFAULT_CHANNEL_WIDTH;
	uint32_t bandwidth = DEFAULT_BANDWIDTH;
        int tone_spacing = 100;
        int freq_est_mask = 0;
        int ext_gain = 0;
        int gains_hex, lna_gain=15, mixer_gain=15, vga_gain=8;      
        int opt_idx = 0;
        struct freedv_advanced adv;
        int use_testframes = 0;
        
        output_bits = 1;
        opt = 0;
        
        while( opt != -1 ){
            static struct option long_opts[] = {
                {"listcodes", no_argument,       0, 'j'},
                {"code",      required_argument, 0, 'k'},
                {"testframes",no_argument,       0, 'i'},
                {"vv",        no_argument,       0, 'l'},
                {"mask",      required_argument, 0, 't'},
                {"filter",    required_argument, 0, 'o'},
                {"injectsnr", no_argument,       0, 'n'},
                {"logsnr",    no_argument,       0, 'L'},
                {0, 0, 0, 0}
            };
        
            opt = getopt_long(argc,argv,"a:d:e:f:g:S:bn:p:s:u:r:Lm:c:M:R:xt:w:jk:vq",long_opts,&opt_idx);
            if (opt != -1) {
                switch (opt) {
		case 'a':
			modem_samp_rate = (uint32_t)atofs(optarg);
			break;
		case 'j':
                        ldpc_codes_list();
                        exit(0);
			break;
		case 'k':
                        fsk_ldpc = 1;
                        adv.codename = optarg;
			break;
		case 'i':
                        if (fsk_ldpc == 0) {
                            fprintf(stderr, "internal testframe mode only supported in --code coded mode!\n");
                            exit(1);
                        }
                        use_testframes = 1;
			break;
		case 'd':
			dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'e':
                        ext_gain = 1;
                        gains_hex = (int)strtol(optarg, NULL, 16);
                        lna_gain = gains_hex >> 8; mixer_gain = (gains_hex >> 4) & 0xf; vga_gain = gains_hex & 0xf;
                        fprintf(stderr, "lna_gain: %d mixer_gain: %d vga_gain: %d\n", lna_gain, mixer_gain, vga_gain);
			break;
		case 'f':
			frequency = (uint32_t)atofs(optarg);
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10); /* tenths of a dB */
                        /* note YMMV - these will vary across RTLSDR brands and samples */
                        if (gain/10 == 49)
                            RxGain = -90.7;
                        else if (gain/10 == 45)
                            RxGain = -85.6;
                        else if (gain/10 == 40)
                            RxGain = -81.1;
                        else if (gain/10 == 35)
                            RxGain = -76.9;
                        else if (gain/10 == 30)
                            RxGain = -71.0;
                        else      
                            fprintf(stderr,"rtl_fsk: WARNING RxLevel not calibrated at this -g gain\n");
			break;
		case 's':
			samp_rate = (uint32_t)atofs(optarg);
			break;
		case 'w':
			bandwidth = (uint32_t)atofs(optarg);
			break;
		case 'p':
			ppm_error = atoi(optarg);
			break;
		case 'q':
                        periodic_output = 1;
			break;
		case 'b':
                        inject_snr = 1;
			break;
		case 'n':
			bytes_to_read = (uint32_t)atof(optarg) * 2;
			break;
		case 'o':
			filter = strtol(optarg, NULL, 0);
			break;
		case 'R':
		case 'r':
			Rs = atoi(optarg);
			break;
		case 'M':
		case 'm':
			M = atoi(optarg);
			break;
		case 'S':
			sync_mode = 1;
			break;
		case 'u':
                        dashboard = 1;
                        strcpy(hostname, optarg);
			break;
		        M = atoi(optarg);                        
			break;
                case 'c':
                        channel_width = atoi(optarg);
                        break;
                case 'x':
                        output_bits = 0;
                        break;
                case 't':
                        freq_est_mask = 1;
                        tone_spacing = atoi(optarg);
                        break;
                case 'v':
                        verbose = 1;
                        break;
                case 'l':
                        verbose = 2;
                        break;
                case 'L':
                        log_snr = 1;
                        break;
                default:
                        usage();
                        break;
		}
            }
	}

	if (argc <= optind) {
		usage();
	} else {
		filename = argv[optind];
	}

	if(out_block_size < MINIMAL_BUF_LENGTH ||
	   out_block_size > MAXIMAL_BUF_LENGTH ){
		fprintf(stderr,
			"Output block size wrong value, falling back to default\n");
		fprintf(stderr,
			"Minimal length: %u\n", MINIMAL_BUF_LENGTH);
		fprintf(stderr,
			"Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
		out_block_size = DEFAULT_BUF_LENGTH;
	}

	buffer = malloc(out_block_size * sizeof(uint8_t)); assert(buffer != NULL);
        nrawbuf_max = 2 * out_block_size;
	rawbuf = malloc(nrawbuf_max * sizeof(uint8_t)); assert(rawbuf != NULL);
        nrawbuf = 0;

        assert((samp_rate % modem_samp_rate) == 0);
        nmodembuf_max = nrawbuf_max*samp_rate/modem_samp_rate;
        modembuf = (COMP*)malloc(nmodembuf_max * sizeof(COMP)); assert(modembuf != NULL);

        /* create UDP socket for dashboard debug/status information ----------------------------- */

        if (dashboard) {
            struct hostent *server;
            
            /* socket: create the socket */
            sockfd = socket(AF_INET, SOCK_DGRAM, 0);
            if (sockfd < 0) {
                fprintf(stderr,"ERROR opening socket");
                exit(1);
            }

            /* gethostbyname: get the server's DNS entry */
            server = gethostbyname(hostname);
            if (server == NULL) {
                fprintf(stderr,"ERROR, no such host as %s\n", hostname);
                exit(1);
            }

            /* build the server's Internet address */
            bzero((char *) &serveraddr, sizeof(serveraddr));
            serveraddr.sin_family = AF_INET;
            bcopy((char *)server->h_addr, 
                  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
            serveraddr.sin_port = htons(portno);
        }
        
        /* continue RTL SDR setup ...... */
        
	if (!dev_given) {
		dev_index = verbose_device_search("0");
	}

	if (dev_index < 0) {
		exit(1);
	}

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif
	/* Set the sample rate */
	verbose_set_sample_rate(dev, samp_rate);

	/* Set the tuner bandwidth */
	verbose_set_bandwidth(dev, bandwidth);

	/* Set the frequency */
	verbose_set_frequency(dev, frequency);

        if (ext_gain == 0) {
            if (0 == gain) {
                /* Enable automatic gain */
		verbose_auto_gain(dev);
            } else {
		/* Enable manual gain */
		gain = nearest_gain(dev, gain);
		verbose_gain_set(dev, gain);
            }
        } else {
            fprintf(stderr, "setting extended gain....\n");
            rtlsdr_set_tuner_gain_ext(dev, lna_gain, mixer_gain, vga_gain);
        }

	verbose_ppm_set(dev, ppm_error);

	if(strcmp(filename, "-") == 0) { /* Write samples to stdout */
		file = stdout;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	} else {
		file = fopen(filename, "wb");
		if (!file) {
			fprintf(stderr, "Failed to open %s\n", filename);
			goto out;
		}
	}

        /* Setup the CSDR Decimator  -------------------------------------------------*/
        {
            // design decimating filter
            float transition_bw = 0.05;
            window_t window = WINDOW_DEFAULT;

            if (samp_rate % modem_samp_rate) {
                fprintf(stderr, "ERROR: samplerate/modem_samplerate = %d/%d must be an integer\n",
                        samp_rate, modem_samp_rate);
                exit(1);
            }
            csdr_factor = samp_rate/modem_samp_rate;
            csdr_taps = csdr_init_decimate_cc(csdr_factor, transition_bw, window, &csdr_padded_taps_length);
            assert(CSDR_BUFSIZE > padded_taps_length);

            // first call to work out how many input samples needed (csdr_nin)
            csdr_decimate_cc(csdr_out, csdr_in, CSDR_BUFSIZE, csdr_taps, csdr_padded_taps_length, csdr_factor, &csdr_nout, &csdr_nin);
        }
        
        /* Setup the FSK demod -------------------------------------------------*/

        if (modem_samp_rate % Rs) {
            fprintf(stderr, "Error: (modem sample rate) / (modem symbol rate) must be an integer\n");
            exit(1);
        }
        if ((modem_samp_rate / Rs) < 8) {
            fprintf(stderr, "Error: (modem sample rate) / (modem symbol rate) must be >= 8\n");
            exit(1);
        }
        if (fsk_ldpc == 0) {
            /* uncoded mode: just set up FSK demod by itself */
            int P = modem_samp_rate/Rs;
            fsk = fsk_create_hbr(modem_samp_rate,Rs,M,P,FSK_DEFAULT_NSYM,FSK_NONE,tone_spacing);
       } else {
            /* coded mode: use FreeDV API to set up and run FSK modem, LDPC, framer */

            int data_bits_per_frame;
            adv.Rs = Rs; adv.Fs = modem_samp_rate; adv.M = M; adv.tone_spacing = tone_spacing;
            freedv = freedv_open_advanced(FREEDV_MODE_FSK_LDPC, &adv);
            assert(freedv != NULL);
            data_bits_per_frame = freedv_get_bits_per_modem_frame(freedv);
            assert(data_bits_ber_frame % 8); /* only want codes that send complete bytes */
            fprintf(stderr, "FSK LDPC mode code: %s data_bits_per_frame: %d\n", adv.codename, data_bits_per_frame);
            fsk = freedv_get_fsk(freedv);
            data_bytes_per_frame = data_bits_per_frame / 8;
            bytes_out = malloc(data_bytes_per_frame); assert(bytes_out != NULL);
            zeros_out = malloc(data_bytes_per_frame); assert(zeros_out != NULL);
            memset(zeros_out, 0, data_bytes_per_frame);
            freedv_set_verbose(freedv, verbose);
            freedv_set_test_frames(freedv, use_testframes);
        }
        fprintf(stderr,"FSK Demod Fs: %5.1f kHz Rs: %3.1f kHz M: %d P: %d Ndft: %d fest_mask: %d\n",
                (float)modem_samp_rate/1000,
                (float)Rs/1000, M, fsk->P, fsk->Ndft, freq_est_mask);
         
        fsk_set_freq_est_alg(fsk, freq_est_mask);
        {    
            /* set minimum "channel" for freq est */
            fsk_lower = Rs/2;
            fsk_upper = 2*M*Rs;
            if (fsk_lower < 2000) fsk_lower = 2000;
            if (fsk_upper < channel_width) fsk_upper = channel_width;
            if (fsk_upper > (int)modem_samp_rate/2) fsk_upper = modem_samp_rate/2;
            fprintf(stderr,"Setting estimator limits to %d to %d Hz.\n", fsk_lower, fsk_upper);
            fsk_set_freq_est_limits(fsk,fsk_lower,fsk_upper);
        }

        /* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(dev);

	if (sync_mode) {
		fprintf(stderr, "Reading samples in sync mode...\n");
		while (!do_exit) {
			r = rtlsdr_read_sync(dev, buffer, out_block_size, &n_read);
			if (r < 0) {
				fprintf(stderr, "WARNING: sync read failed.\n");
				break;
			}

			if ((bytes_to_read > 0) && (bytes_to_read < (uint32_t)n_read)) {
				n_read = bytes_to_read;
				do_exit = 1;
			}

			if (fwrite(buffer, 1, n_read, file) != (size_t)n_read) {
				fprintf(stderr, "Short write, samples lost, exiting!\n");
				break;
			}

			if ((uint32_t)n_read < out_block_size) {
				fprintf(stderr, "Short read, samples lost, exiting!\n");
				break;
			}

			if (bytes_to_read > 0)
				bytes_to_read -= n_read;
		}
	} else {
		fprintf(stderr, "Reading samples in async mode...\n");
		r = rtlsdr_read_async(dev, rtlsdr_callback, (void *)file,
				      0, out_block_size);
	}

	if (do_exit)
		fprintf(stderr, "\nUser cancel, exiting...\n");
	else
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);

	if (file != stdout)
		fclose(file);

	rtlsdr_close(dev);
        if (fsk_ldpc == 0)
            fsk_destroy(fsk);
        else {
            free(bytes_out);
            freedv_close(freedv);
        }
        
	free (buffer);
	free (rawbuf);
        free(modembuf);
out:
	return r >= 0 ? r : -r;
}
