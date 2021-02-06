// hpctrl.cpp: simple command-line utility
// to communicate with HP8753 and HP4191 over GPIB USB device
// it is based on the vna.cpp from John Miles, KE5FX 
//  http://www.ke5fx.com/gpib/readme.htm
// and depends on gpiblib.dll
// it compiles with MSVS2019 community version.
// the code is quite dirty, sorry.

#include <stdio.h>
#include <inttypes.h>
#include <malloc.h>
#include <float.h>
#include <math.h>
#include <windows.h>

#include "gpiblib.h"
#include <cassert>

#define FTOI(x) (S32((x)+0.5))

#define ACTION_QUEUE_SIZE 256

int session_logging = 0;
HANDLE gpib_mutex;

C8* timestamp(void);

void log_session(const char* a, const char* b)
{
    if (session_logging)
    {
        FILE* f = fopen("log_session.txt", "a+");
        const char* logmsg = b + strlen(b) - 1;
        char* logm = 0;
        if ((*logmsg == '\n') || (*logmsg == '\r'))
        {
            char* logm = (char*)malloc(strlen(b) + 1);
            strcpy(logm, b);
            logmsg = logm;
            logm += strlen(logm) - 1;
            while ((logm > logmsg) && ((*logm == '\n') || (*logm == '\r')))
                *(logm--) = 0;
        }
        else logmsg = b;
        fprintf(f, "%s %s '%s'\n", timestamp(), a, logmsg);
        fclose(f);
        if (logm != 0) free(logm);
    }
}

static int cmdline_i = 0;
static int cmdline_a = 16;
static int cmdline_s11 = 0;
static int cmdline_s21 = 0;
static int cmdline_s12 = 0;
static int cmdline_s22 = 0;

static volatile int running = 1;
static volatile int autosweep = 0;
static char *save_file_name = 0;

enum sending_format { form1, form4 };
static sending_format current_sending_format = form1;

enum measure_format { fmt_ri, fmt_ma, fmt_db };
static measure_format current_format = fmt_ri;

enum freq_format { hz, khz, mhz, ghz };
static freq_format current_freq_format = ghz;

enum input_mode { mode_menu, mode_cmd, mode_input_blocked };

static volatile input_mode current_input_mode = mode_menu;
static volatile int new_input_entered;

static char ln[52];

enum action_type {
    action_none, action_connect, action_disconnect, action_sweep, action_getstate, action_setstate,
    action_getcalib, action_setcalib, action_cmd_puts, action_cmd_query, action_cmd_status, action_cmd_read_asc,
    action_cmd_continuous_asc, action_cmd_repeated_asc, action_cmd_read_bin, action_exit, action_reset, action_freset,
    action_s11, action_s12, action_s21, action_s22, action_all, action_clear, action_form, action_fmt, action_freq,
    action_file, action_autosweep
};

static action_type action_queue[ACTION_QUEUE_SIZE];
static char* action_arguments[ACTION_QUEUE_SIZE];
static int aq_wp, aq_rp;
static HANDLE aq_lock;

static volatile action_type action;
static int cmd_read_repeat_count;


HANDLE action_event;


void WINAPI GPIB_error(C8* msg, S32 ibsta, S32 iberr, S32 ibcntl)
{
    printf("GPIB Error %s, ibsta=%d, iberr=%d, ibcntl=%d\n", msg, ibsta, iberr, ibcntl);
    exit(1);
}

void gpib_lock()
{
    DWORD dwWaitResult;
    do {
        dwWaitResult = WaitForSingleObject(gpib_mutex,    // handle to mutex
            INFINITE);  // no time-out interval
    } while (dwWaitResult != WAIT_OBJECT_0);
}

void gpib_unlock()
{
    if (!ReleaseMutex(gpib_mutex))
        printf("error releasing mutex: %d\n", GetLastError());
}

int gpib_trylock()
{
    DWORD dwWaitResult = WaitForSingleObject(gpib_mutex, 1);
    if (dwWaitResult == WAIT_OBJECT_0) return 1;
    return 0;
}

void conv_RI_2_MA(double r, double i, double *m, double *a)
{
    *m = sqrt(r * r + i * i);

    if (*m > 1E-20)
        *a = atan2(i, r) * RAD2DEG;
    else
        *a = 0.0;
}

void conv_RI_2_DB(double r, double i, double *dB, double *a)
{
    *dB = 20.0 * log10(max(1E-15, sqrt(r * r + i * i)));

    if (*dB > -200.0)
        *a = atan2(i, r) * RAD2DEG;
    else
        *a = 0.0;
}

void print8bit(uint8_t b)
{
    for (int i = 0; i < 8; i++)
        printf("%d", (b >> (7 - i)) & 1);
}

void conv_form1_2_RI(uint8_t* data, double* r, double* i)
{    
    // HP8753 form1 binary internal representation format: 6 bytes: 
    // 0:  imag mantissa1 (MSB)
    // 1:  imag mantissa2 (LSB)
    // 2:  real mantissa1 (MSB)
    // 3:  real mantissa2 (LSB)
    // 4:  "additional resolution for transferring raw data" - no idea what this is, always saw 0 there
    // 5:  common exponent
    //
    // imag mantissa is 16-bit signed integer (2-compl)
    // real mantissa is 16-bit signed integer (2-compl)
    // common exponent is 8-bit signed integer (2-compl)
    //
    // imag = (imag mantissa/32768) * 2^(common exponent)
    // real = (real mantissa/32768) * 2^(common exponent)

    int8_t common_exp = (int8_t)*(data + 5);
    double exp = pow(2, common_exp);
    uint16_t i_bits = (((uint16_t) *data) << 8) + (uint16_t) *(data + 1);
    int16_t i_value = (int16_t)i_bits;
    uint16_t r_bits = (((uint16_t) * (data + 2)) << 8) + (uint16_t) * (data + 3);
    int16_t r_value = (int16_t)r_bits;
    *i = ((double)i_value) / (double)32768 * exp;
    *r = ((double)r_value) / (double)32768 * exp;
}

S32 S16_BE(C8* s)
{
    U8* p = (U8*)s;
    return (((S32)p[0]) << 8) + (S32)p[1];
}

C8* time_text(S64 time_uS)
{
    FILETIME   ftime;
    FILETIME   lftime;
    SYSTEMTIME stime;

    S64 file_time = time_uS * 10;

    ftime.dwLowDateTime = S32(file_time & 0xffffffff);
    ftime.dwHighDateTime = S32(U64(file_time) >> 32);

    FileTimeToLocalFileTime(&ftime, &lftime);

    FileTimeToSystemTime(&lftime, &stime);

    static C8 text[1024];

    GetTimeFormatA(LOCALE_SYSTEM_DEFAULT,
        0,
        &stime,
        NULL,
        text,
        sizeof(text));

    return text;
}

//***************************************************************************
//
// date_text()
//
//***************************************************************************

C8* date_text(S64 time_uS)
{
    FILETIME   ftime;
    FILETIME   lftime;
    SYSTEMTIME stime;

    S64 file_time = time_uS * 10;

    ftime.dwLowDateTime = S32(file_time & 0xffffffff);
    ftime.dwHighDateTime = S32(U64(file_time) >> 32);

    FileTimeToLocalFileTime(&ftime, &lftime);

    FileTimeToSystemTime(&lftime, &stime);

    static C8 text[1024];

    GetDateFormatA(LOCALE_SYSTEM_DEFAULT,
        0,
        &stime,
        NULL,
        text,
        sizeof(text));

    return text;
}

C8* timestamp(void)
{
    static union
    {
        FILETIME ftime;
        S64      itime;
    }
    T;

    GetSystemTimeAsFileTime(&T.ftime);

    static C8 text[1024];

    sprintf(text,
        "%s %s",
        date_text(T.itime / 10),
        time_text(T.itime / 10));

    return text;
}

int DC_entry = 0;
DOUBLE R_ohms = 50;
C8 instrument_name[512];

bool is_8753(void)
{
    return (strstr(instrument_name, "8753") != NULL) ||
           (strstr(instrument_name, "8702") != NULL) ||
           (strstr(instrument_name, "8703") != NULL);
}

bool is_8752(void)
{
    return (strstr(instrument_name, "8752") != NULL);
}

bool is_8720(void)
{
    return (strstr(instrument_name, "8719") != NULL) ||
        (strstr(instrument_name, "8720") != NULL) ||
        (strstr(instrument_name, "8722") != NULL);
}

bool is_8510(void)
{
    return (strstr(instrument_name, "8510") != NULL);
}

bool is_8510C(void)
{
    return (strstr(instrument_name, "8510C") != NULL);
}

void instrument_setup(bool debug_mode = TRUE)
{
    if (debug_mode)
    {
        log_session("p>", "DEBUON;");

        gpib_lock();
        GPIB_printf("DEBUON;");
        gpib_unlock();
    }

    log_session("q>", "OUTPIDEN");

    gpib_lock();
    C8* data = GPIB_query("OUTPIDEN");
    gpib_unlock();
    _snprintf(instrument_name, sizeof(instrument_name) - 1, "%s", data);
    instrument_name[sizeof(instrument_name) - 1] = 0;
    log_session("q:", instrument_name);
    if (strlen(instrument_name) > 0)
        if (instrument_name[strlen(instrument_name) - 1] == '\n')
            instrument_name[strlen(instrument_name) - 1] = 0;
    //printf("instrument name: %s\n", data);   
    log_session("p>", "HOLD;");

    gpib_lock();
    GPIB_printf("HOLD;");
    gpib_unlock();
}

bool read_complex_trace(const C8* param,
    const C8* query,
    COMPLEX_DOUBLE* dest,
    S32             cnt,
    S32             progress_fraction)
{
    U8 mask = 0x40;

    if (is_8753()) {
        log_session("p>", "CLES;SRE 4;ESNB 1;");
        gpib_lock();
        GPIB_printf("CLES;SRE 4;ESNB 1;"); mask = 0x40;
        gpib_unlock();
    }  // Extended register bit 0 = SING sweep complete; map it to status bit and enable SRQ on it 
    if (is_8752()) {
        log_session("p>", "CLES;SRE 4;ESNB 1;");
        gpib_lock();
        GPIB_printf("CLES;SRE 4;ESNB 1;"); mask = 0x40;
        gpib_unlock();
    }
    if (is_8720()) {
        log_session("p>", "CLES;SRE 4;ESNB 1;");
        GPIB_printf("CLES;SRE 4;ESNB 1;"); mask = 0x40;
    }
    if (is_8510()) {
        log_session("p>", "CLES;");
        gpib_lock();
        GPIB_printf("CLES;");              mask = 0x10;
        gpib_unlock();
    }

    if (current_sending_format == form4)
    {
        log_session("p>", param);
        log_session("p>", "FORM4;SING;");
        gpib_lock();
        GPIB_printf("%s;FORM4;SING;", param);
        gpib_unlock();
    }
    else if (current_sending_format == form1)
    {
        log_session("p>", param);
        log_session("p>", "FORM1;SING;");
        gpib_lock();
        GPIB_printf("%s;FORM1;SING;", param);
        gpib_unlock();
    }

    if (is_8510())             // Skip first reading to ensure EOS bit is clear, but only on 8510 
    {                       // (SRQ bit auto-resets after the first successful poll on 8753)   
        GPIB_serial_poll();
    }

    S32 st = timeGetTime();

    for (;;)
    {
        Sleep(1);

        gpib_lock();
        U8 result = GPIB_serial_poll();
        gpib_unlock();

        if (result & mask)
        {
            printf("! %s sweep finished in %d ms\n", param, timeGetTime() - st);
            break;
        }
    }

    if (is_8753()) { 
        log_session("p>", "CLES;SRE 0;");
        gpib_lock();
        GPIB_printf("CLES;SRE 0;"); 
        gpib_unlock();
    }
    if (is_8752()) { 
        log_session("p>", "CLES;SRE 0;");
        gpib_lock();
        GPIB_printf("CLES;SRE 0;"); 
        gpib_unlock();
    }
    if (is_8720()) { 
        log_session("p>", "CLES;SRE 0;");
        gpib_lock();
        GPIB_printf("CLES;SRE 0;");
        gpib_unlock();
    }
    if (is_8510()) { 
        log_session("p>", "CLES;");
        gpib_lock();
        GPIB_printf("CLES;"); 
        gpib_unlock();
    }

    log_session("p>", query);
    gpib_lock();
    GPIB_printf("%s;", query);
    gpib_unlock();

    if (current_sending_format == form4)
    {
        for (S32 i = 0; i < cnt; i++)
        {
            gpib_lock();
            C8* data = GPIB_read_ASC(-1, FALSE);
            gpib_unlock();
            log_session("a:", data);

            DOUBLE I = DBL_MIN;
            DOUBLE Q = DBL_MIN;

            sscanf(data, "%lf, %lf", &I, &Q);

            if ((I == DBL_MIN) || (Q == DBL_MIN))
            {
                printf("Error VNA read timed out reading %s (point %d of %d points)", param, i, cnt);
                return FALSE;
            }

            dest[i].real = I;
            dest[i].imag = Q;
        }
    }
    else if (current_sending_format == form1)
    {
        //
         // Verify its header 
         //

        S32 actual = 0;
        gpib_lock();
        uint8_t* data = (uint8_t*)GPIB_read_BIN(2, TRUE, FALSE, &actual);
        gpib_unlock();
        if (actual != 2)
        {
            printf("Error: data header query returned %d bytes", actual);
            return 0;
        }
        log_session("b:", "...");

        if ((data[0] != '#') || (data[1] != 'A'))
        {
            printf("Error: data FORM1 block header was 0x%.2X 0x%.2X", data[0], data[1]);
            return 0;
        }

        //
        // Get length in bytes
        //

        actual = 0;
        gpib_lock();
        data = (uint8_t*)GPIB_read_BIN(2, TRUE, FALSE, &actual);
        gpib_unlock();

        S32 len = S16_BE((C8*)data);

        if (actual != 2)
        {
            printf("Error: data len returned %d bytes", actual);
            return 0;
        }
        log_session("b:", "...");

        //
        // Get data
        //

        gpib_lock();
        data = (uint8_t*)GPIB_read_BIN(len, TRUE, FALSE, &actual);
        gpib_unlock();

        if (actual != len)
        {
            printf("Error: data len = %d, received %d", len, actual);
            return 0;
        }

        if (len != cnt * 6)
        {
            printf("Error: data cnt = %d, cnt*6 = %d, but len = %d", cnt, cnt * 6, len);
            return 0;
        }
        log_session("b:", "...");

        for (int i = 0; i < cnt; i++)
            conv_form1_2_RI(data + 6 * i, &dest[i].real, &dest[i].imag);
    }

    return 1;
}

static int connected = 0;

static S32 n_AC_points;
static S32 first_AC_point;
static bool include_DC;
static S32 n_alloc_points;
static DOUBLE  *freq_Hz;
COMPLEX_DOUBLE *S11, *S21, *S12, *S22;
static double start_Hz, stop_Hz;

int connect()
{
    if (connected) return 1;
    S32 addr = cmdline_a;
    
    GPIB_connect(addr,       // device_address
        GPIB_error, // handler
        FALSE,      // clear
        -1,          // timeout_msecs=-1 to disable
        -1,          // board_address
        FALSE,      // release_system_control
        -1,          // max_read_buffer_len
        FALSE);     // Disable Prologix auto-read mode at startup
    
    GPIB_set_EOS_mode(10);
    GPIB_set_serial_read_dropout(10000);
    instrument_setup();

    connected = 1;
    return 1;
}

void output_complex_value(FILE *f, COMPLEX_DOUBLE *val)
{
    double v1 = 0, v2 = 0;

    switch (current_format)
    {
    case fmt_ri: 
        v1 = val->real;
        v2 = val->imag;
        break;
    case fmt_ma:         
        conv_RI_2_MA(val->real, val->imag, &v1, &v2);        
        break;
    case fmt_db:         
        conv_RI_2_DB(val->real, val->imag, &v1, &v2);
        break;
    }
    fprintf(f, " %.6lf %.6lf", v1, v2);
}

const char* freq_str()
{
    switch (current_freq_format)
    {
    case hz: return "HZ";
    case khz: return "KHZ";
    case mhz: return "MHZ";
    case ghz: return "GHZ";
    }
    return "???";
}

const char* fmt_str()
{
    switch (current_format)
    {
    case fmt_ri: return "RI";
    case fmt_ma: return "MA";
    case fmt_db: return "DB";    
    }
    return "??";
}

void save_file(FILE *of, const char *fname)
{
    if (fname) fprintf(of, "!\n! %s\n!\n", fname);
    fprintf(of, "! Touchstone 1.1 file saved by HPCTRL.EXE\n"
        "! %s\n"
        "!\n"
        "!    Source: %s\n", timestamp(), instrument_name);
    fprintf(of, "!  Min freq: %e Hz\n", start_Hz);
    fprintf(of, "!  Max freq: %e Hz\n", stop_Hz);
    fprintf(of, "!    Points: %d\n", n_alloc_points);
    fprintf(of, "!    Params:%s%s%s%s\n",
        cmdline_s11 ? " S11" : "", cmdline_s21 ? " S21" : "", cmdline_s12 ? " S12" : "", cmdline_s22 ? " S22" : "");
    fprintf(of, "!\n!!!!!\n# %s S %s R 50\n", freq_str(), fmt_str());
    for (S32 i = 0; i < n_alloc_points; i++)
    {
        double freq_frac = 0;
        switch (current_freq_format)
        {
        case hz: freq_frac = 1.0; break;
        case khz: freq_frac = 1E3; break;
        case mhz: freq_frac = 1E6; break;
        case ghz: freq_frac = 1E9; break;
        }
        fprintf(of, "%.16lf", freq_Hz[i] / freq_frac);
        if (cmdline_s11) output_complex_value(of, S11 + i);
        if (cmdline_s21) output_complex_value(of, S21 + i);
        if (cmdline_s12) output_complex_value(of, S12 + i);
        if (cmdline_s22) output_complex_value(of, S22 + i);
        fprintf(of, "\n");
    }
}

int sweep()
{
    if (!connected)
    {
        printf("!not connected\n");
        return 0;
    }
    log_session("q>", "FORM4;STAR;OUTPACTI;");
    gpib_lock();
    C8* data = GPIB_query("FORM4;STAR;OUTPACTI;");
    gpib_unlock();
    sscanf(data, "%lf", &start_Hz);
    log_session("q:", data);
    log_session("q>", "STOP;OUTPACTI;");
    gpib_lock();
    data = GPIB_query("STOP;OUTPACTI;");
    gpib_unlock();
    sscanf(data, "%lf", &stop_Hz);
    log_session("q:", data);

    log_session("q>", "POIN;OUTPACTI;");

    gpib_lock();
    data = GPIB_query("POIN;OUTPACTI;");
    gpib_unlock();
    DOUBLE fn = 0.0;
    sscanf(data, "%lf", &fn);
    log_session("q:", data);
    S32 n = (S32)(fn + 0.5);

    if ((n < 1) || (n > 1000000))
    {
        GPIB_disconnect();
        printf("Error n_points = %d\n", n);
        return 0;
    }

    //
    // Reserve space for DC term if requested
    // 
    include_DC = (DC_entry != 0);

    n_alloc_points = n;
    n_AC_points = n;
    first_AC_point = 0;

    if (include_DC)
    {
        n_alloc_points++;
        first_AC_point = 1;
    }

    freq_Hz = (DOUBLE*)malloc(n_alloc_points * sizeof(freq_Hz[0])); memset(freq_Hz, 0, n_alloc_points * sizeof(freq_Hz[0]));

    S11 = (COMPLEX_DOUBLE*)malloc(n_alloc_points * sizeof(S11[0])); memset(S11, 0, n_alloc_points * sizeof(S11[0]));
    S21 = (COMPLEX_DOUBLE*)malloc(n_alloc_points * sizeof(S21[0])); memset(S21, 0, n_alloc_points * sizeof(S21[0]));
    S12 = (COMPLEX_DOUBLE*)malloc(n_alloc_points * sizeof(S12[0])); memset(S12, 0, n_alloc_points * sizeof(S12[0]));
    S22 = (COMPLEX_DOUBLE*)malloc(n_alloc_points * sizeof(S22[0])); memset(S22, 0, n_alloc_points * sizeof(S22[0]));

    if (include_DC)
    {
        S11[0].real = 1.0;
        S21[0].real = 1.0;
        S12[0].real = 1.0;
        S22[0].real = 1.0;
    }
    //
    // Construct frequency array
    //
    // For 8510 and SCPI, we support only linear sweeps (8510 does not support log sweeps, only lists)
    //
    // For non-8510 analyzers, if LINFREQ? indicates a linear sweep is in use, we construct 
    // the array directly.  If a nonlinear sweep is in use, we obtain the frequencies from
    // an OUTPLIML query (08753-90256 example 3B).
    //
    // Note that the frequency parameter in .SnP files taken in POWS or CWTIME mode
    // will reflect the power or time at each point, rather than the CW frequency
    //

    bool lin_sweep = TRUE;

    if (!is_8510())
    {
        log_session("q>", "LINFREQ?;");
        gpib_lock();
        char* qresp = GPIB_query("LINFREQ?;");
        gpib_unlock();
        lin_sweep = (qresp[0] == '1');
        log_session("q:", qresp);
    }

    if (lin_sweep)
    {
        for (S32 i = 0; i < n_AC_points; i++)
        {
            freq_Hz[i + first_AC_point] = start_Hz + (((stop_Hz - start_Hz) * i) / (n_AC_points - 1));
        }
    }
    else
    {
        log_session("p>", "OUTPLIML;");
        gpib_lock();
        GPIB_printf("OUTPLIML;");
        gpib_unlock();

        for (S32 i = 0; i < n_AC_points; i++)
        {
            gpib_lock();
            C8* data = GPIB_read_ASC(-1, FALSE);
            gpib_unlock();
            log_session("a:", data);

            DOUBLE f = DBL_MIN;
            sscanf(data, "%lf", &f);

            if (f == DBL_MIN)
            {
                printf("Error, VNA read timed out reading OUTPLIML (point %d of %d points)", i, n_AC_points);
                GPIB_disconnect();
                return 0;
            }

            freq_Hz[i + first_AC_point] = f;
        }
    }

    //
    // If this is an 8753 or 8720, determine what the active parameter is so it can be
    // restored afterward
    //
    // (S12 and S22 queries are not supported on 8752 or 8510)
    //

    S32 active_param = 0;
    C8 param_names[4][4] = { "S11", "S21", "S12", "S22" };

    if (is_8753() || is_8720())
    {
        for (active_param = 0; active_param < 4; active_param++)
        {
            C8 text[512] = { 0 };
            _snprintf(text, sizeof(text) - 1, "%s?", param_names[active_param]);

            log_session("q>", text);
            gpib_lock();
            char* qstr = GPIB_query(text);
            gpib_unlock();
            log_session("q:", qstr);
            if (qstr[0] == '1')
            {
                break;
            }
        }
    }

    //
    // Read data from VNA
    //

    int sweep_iteration = 0;

    do {
        int req_results = 0;
        int ok_results = 0;

        if (cmdline_s11) req_results++;
        if (cmdline_s12) req_results++;
        if (cmdline_s21) req_results++;
        if (cmdline_s22) req_results++;

        if (cmdline_s11 && read_complex_trace("S11", "OUTPDATA", &S11[first_AC_point], n_AC_points, 20)) ok_results++;
        if (cmdline_s21 && read_complex_trace("S21", "OUTPDATA", &S21[first_AC_point], n_AC_points, 40)) ok_results++;
        if (cmdline_s12 && read_complex_trace("S12", "OUTPDATA", &S12[first_AC_point], n_AC_points, 60)) ok_results++;
        if (cmdline_s22 && read_complex_trace("S22", "OUTPDATA", &S22[first_AC_point], n_AC_points, 80)) ok_results++;

        if ((ok_results == req_results) && (req_results))
        {
            double min_Hz = include_DC ? 0.0 : start_Hz;
            double max_Hz = stop_Hz;
            double Zo = R_ohms;

            save_file(stdout, 0);

            if (save_file_name)
            {
                char* real_file_name = 0;
                FILE* sf = 0;
                if (autosweep)
                {
                    real_file_name = (char*)malloc(strlen(save_file_name) + 15);
                    sprintf(real_file_name, "%04d_%s", sweep_iteration, save_file_name);
                }
                else real_file_name = save_file_name;
                sf = fopen(real_file_name, "w+");
                if (!sf)
                    printf("!could not open file %s for writing\n", real_file_name);
                else
                {
                    save_file(sf, real_file_name);
                    fclose(sf);
                }
                if (autosweep) free(real_file_name);
            }
        }
        sweep_iteration++;

    }  while (autosweep && running);

    //
    // Restore active parameter and exit
    //

    if ((is_8753() || is_8720())
        &&
        (active_param <= 3))
    {
        log_session("p>", param_names[active_param]);
        gpib_lock();
        GPIB_printf(param_names[active_param]);
        gpib_unlock();
        Sleep(500);
    }

    log_session("p>", "DEBUOFF;CONT;");
    gpib_lock();
    GPIB_printf("DEBUOFF;CONT;");
    gpib_unlock();

    free(freq_Hz);
    free(S11);
    free(S21);
    free(S12);
    free(S22);

    printf("\n");
    fflush(stdout);
    if (save_file_name) free(save_file_name);
    save_file_name = 0;
    return 1;
}

void disconnect()
{
    Sleep(500);
    GPIB_disconnect();
    connected = 0;
}

void getcalib()
{
    uint8_t* data;
    S32 len;
    S32 actual = 0;
    int CALIONE2 = 1;
    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }
    Sleep(500);

    log_session("w>", "FORM1;");
    gpib_lock();
    GPIB_write("FORM1;");
    gpib_unlock();

    if (is_8510())
    {
        //
        // Determine active HP 8510 calibration type and set #
        //

        const C8* types[] = { "RESPONSE", "RESPONSE & ISOL'N", "S11 1-PORT", "S22 1-PORT", "2-PORT" };
        const C8* names[] = { "CALIRESP", "CALIRAI",           "CALIS111",   "CALIS221",   "CALIFUL2" };
        const S32 cnts[] = { 1,          2,                   3,            3,            12 };

        S32 active_cal_type = -1;
        S32 active_cal_set = -1;

        S32 n_types = ARY_CNT(cnts);

        log_session("q>", "CALI?;");
        gpib_lock();
        C8* result = GPIB_query("CALI?;");
        gpib_unlock();
        log_session("q:", result);
        C8* term = strrchr(result, '"');                          // Remove leading and trailing quotes returned by 8510

        if (term != NULL)
        {
            *term = 0;
            result++;
        }

        for (S32 i = 0; i < n_types; i++)
        {
            if (!_stricmp(result, types[i]))
            {
                active_cal_type = i;
                log_session("q>", "CALS?;");
                gpib_lock();
                char* qstr = GPIB_query("CALS?;");
                gpib_unlock();
                log_session("q:", qstr);
                active_cal_set = atoi(qstr);
                break;
            }
        }

        if ((active_cal_type != -1) && (active_cal_set > 0))
        {
            const C8* active_cal_name = names[active_cal_type];   // Get name of active calibration type
            S32       n_arrays = cnts[active_cal_type];    // Get # of data arrays for active calibration type

            log_session("q>", "POIN;OUTPACTI;");
            gpib_lock();
            data = (uint8_t*)GPIB_query("POIN;OUTPACTI;");                  // Get # of points in calibrated trace 
            gpib_unlock();
            log_session("q:", (char *)data);
            DOUBLE fnpts = 0.0;
            sscanf((C8*)data, "%lf", &fnpts);
            S32 trace_points = FTOI(fnpts);

            if ((trace_points < 1) || (trace_points > 1000000))
            {
                printf("Error: trace_points = %d\n", trace_points);
                return;
            }

            S32 array_bytes = trace_points * 6;
            S32 total_bytes = array_bytes * n_arrays;

            printf("CORROFF;HOLD;FORM1;%s;", active_cal_name);

            //
            // For each array....
            // 

            S32 n = 0;

            for (S32 j = 0; j < n_arrays; j++)
            {
                n += array_bytes;

                char str[30];
                sprintf(str, "FORM1;OUTPCALC%d%d;", (j + 1) / 10, (j + 1) % 10);
                log_session("q>", str);
                gpib_lock();
                GPIB_printf(str);
                data = (uint8_t*)GPIB_read_BIN(2);
                gpib_unlock();

                log_session("b:", "...");

                if ((data[0] != '#') || (data[1] != 'A'))
                {
                    printf("Error: OUTPCALC FORM1 block header was 0x%.2X 0x%.2X", data[0], data[1]);
                    return;
                }

                U16 H = *(U16*)data;

                gpib_lock();
                data = (uint8_t*)GPIB_read_BIN(2);
                gpib_unlock();

                len = S16_BE((C8*)data);

                if (len != array_bytes)
                {
                    printf("Error: OUTPCALC returned %d bytes, expected %d", len, array_bytes);
                    return;
                }
                log_session("b:", "...");


                U16 N = *(U16*)data;

                printf("%d\n", H);
                printf("%d\n", N);
                printf("INPUCALC%d%d;", (j + 1) / 10, (j + 1) % 10);

                gpib_lock();
                C8* IQ = GPIB_read_BIN(array_bytes, TRUE, FALSE, &actual);
                gpib_unlock();
                log_session("b:", "...");

                if (actual != array_bytes)
                {
                    printf("Error: OUTPCALC%d%d returned %d bytes, expected %d", (j + 1) / 10, (j + 1) % 10, actual, array_bytes);
                    return;
                }

                printf("%d\n", array_bytes);
                for (int i = 0; i < array_bytes; i++)
                {
                    printf("%02x", IQ[i]);
                    if ((i % 40 == 39) && (i < array_bytes - 1)) printf("\n");
                }
                printf("\n");
            }

            assert(n == total_bytes);

            printf("SAVC; CALS%d; CORROFF;", active_cal_set);

            //
            // Write second copy of learn string command to file
            //
            // On the 8510, the learn string state must be restored before its 
            // accompanying calibration data.  Otherwise, SAVC will incorrectly 
            // associate the prior instrument state with the cal set's limited 
            // instrument state.  This would be fine except that the CALIxxxx commands 
            // switch to single-parameter display mode, regardless of what was saved 
            // in the learn string.
            //
            // So, we work around this lameness by recording the learn string twice, once
            // before the calibration data and again after it...
        }
    }
    else
    {
        //
        // Determine active HP 8753 calibration type
        //
        // TODO: what about CALIONE2? (and CALITRL2 and CALRCVR on 8510?)
        //
        //   8510-90280 p. C-13: CALIONE2 not recommended for use with S-param sets, since the same
        //   forward error terms will be used in both directions.  Use CALIONE2 with T/R sets.
        //
        //   8510-90280 p. PC-41: CALI? apparently does not return CALITRL or CALIONE2
        //              p. I-4: If INPUCALC'ing CALIONE2, you must issue CALIFUL2 with all 12 coeffs
        //
        // Note: on 8753A/B/C at least, only the active channel's calibration type query returns '1'.
        // E.g., CALIS111 and CALIS221 may both be valid and available, but only one will be 
        // saved with the instrument state.  (Or neither, if an uncalibrated response parameter is 
        // being displayed in the active channel.)
        //
        // V1.56: added CALIONE2 for 8753C per G. Anagnostopoulos email of 25-Apr-16
        // V1.57: added check box for CALIONE2 to avoid 8719C lockups, per M. Swanberg email of 28-Oct-16
        //

        const C8* types[] = { "CALIRESP", "CALIRAI", "CALIS111", "CALIS221", "CALIFUL2", "CALIONE2" };
        const S32 cnts[] = { 1,          2,         3,          3,          12,         12 };

        S32 active_cal_type = -1;
        S32 n_types = ARY_CNT(cnts);

        if (!CALIONE2)
        {
            n_types--;
        }

        char str[40];

        for (S32 i = 0; i < n_types; i++)
        {
            sprintf(str, "%s?;", types[i]);
            log_session("p>", str);
            GPIB_printf(str);

            gpib_lock();
            C8* result = GPIB_read_ASC();
            gpib_unlock();

            log_session("a:", result);


            if (result[0] == '1')
            {
                active_cal_type = i;
                break;
            }
        }

        if (active_cal_type != -1)
        {
            const C8* active_cal_name = types[active_cal_type];   // Get name of active calibration type
            S32       n_arrays = cnts[active_cal_type];    // Get # of data arrays for active calibration type

            log_session("q>", "FORM3;POIN?;");

            gpib_lock();
            data = (uint8_t*)GPIB_query("FORM3;POIN?;");                    // Get # of points in calibrated trace
            gpib_unlock();

            log_session("q:", (char *)data);
            DOUBLE fnpts = 0.0;
            sscanf((C8*)data, "%lf", &fnpts);
            S32 trace_points = FTOI(fnpts);

            S32 array_bytes = trace_points * 6;
            S32 total_bytes = array_bytes * n_arrays;

            S32 cmdlen = 15 + strlen(active_cal_name);             // Write command to file that will select this calibration type

            //printf("CORROFF;FORM1;%s;", active_cal_name);
            printf("%s\n", active_cal_name);
            printf("%d\n", n_arrays);
            //
            // For each array....
            // 

            S32 n = 0;

            for (S32 j = 0; j < n_arrays; j++)
            {
                n += array_bytes;

                sprintf(str, "FORM1;OUTPCALC%d%d;", (j + 1) / 10, (j + 1) % 10);
                log_session("p>", str);
                gpib_lock();
                GPIB_printf(str);
                data = (uint8_t*)GPIB_read_BIN(2);
                gpib_unlock();

                if ((data[0] != '#') || (data[1] != 'A'))
                {
                    printf("Error: OUTPCALC FORM1 block header was 0x%.2X 0x%.2X", data[0], data[1]);
                    return;
                }

                U16 H = *(U16*)data;

                gpib_lock();
                data = (uint8_t*)GPIB_read_BIN(2);
                gpib_unlock();

                len = S16_BE((C8*)data);

                if (len != array_bytes)
                {
                    printf("Error: OUTPCALC returned %d bytes, expected %d", len, array_bytes);
                    fflush(stdout);
                    return;
                }
                log_session("b:", "...");

                U16 N = *(U16*)data;

                //printf("INPUCALC%d%d;", (j + 1) / 10, (j + 1) % 10);

                gpib_lock();
                uint8_t* IQ = (uint8_t*)GPIB_read_BIN(array_bytes, TRUE, FALSE, &actual);
                gpib_unlock();

                if (actual != array_bytes)
                {
                    printf("Error: OUTPCALC%d%d returned %d bytes, expected %d", (j + 1) / 10, (j + 1) % 10, actual, array_bytes);
                    fflush(stdout);
                    return;
                }
                log_session("b:", "...");

                printf("%d\n", array_bytes);
                for (int i = 0; i < array_bytes; i++)
                {
                    printf("%02x", IQ[i]);
                    if ((i % 40 == 39) && (i < array_bytes - 1)) printf("\n");
                }
                printf("\n");
            }

            assert(n == total_bytes);

            // printf("OPC?;SAVC;\n");
        }
    }

    printf("\n");
    fflush(stdout);

    Sleep(500);
    log_session("p>", "DEBUOFF;CONT;");

    gpib_lock();
    GPIB_printf("DEBUOFF;CONT;");
    gpib_unlock();

}

void getstate()
{
    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }
    
    //
    // Request learn string
    //

    C8 learn_string[65536] = { 0 };
    S32 learn_string_size = 0;

    U16 learn_N = 0;
    U16 learn_H = 0;

    Sleep(500);

    log_session("w>", "FORM1;OUTPLEAS;");
    gpib_lock();
    GPIB_write("FORM1;OUTPLEAS;");

    //
    // Verify its header 
    //

    S32 actual = 0;
    uint8_t *data = (uint8_t *)GPIB_read_BIN(2, TRUE, FALSE, &actual);
    gpib_unlock();


    if (actual != 2)
    {
        printf("Error: OUTPLEAS header query returned %d bytes", actual);
        fflush(stdout);
        return;
    }

    if ((data[0] != '#') || (data[1] != 'A'))
    {
        printf("Error: OUTPLEAS FORM1 block header was 0x%.2X 0x%.2X", data[0], data[1]);
        fflush(stdout);
        return;
    }

    learn_H = *(U16*)data;

    printf("%02x%02x", data[0], data[1]);

    //
    // Get length in bytes of learn string
    //

    actual = 0;
    gpib_lock();
    data = (uint8_t *)GPIB_read_BIN(2, TRUE, FALSE, &actual);

    gpib_unlock();
    log_session("b:", "...");

    S32 len = S16_BE((C8 *)data);

    if (actual != 2)
    {
        printf("Error: OUTPLEAS string len returned %d bytes", actual);
        return;
    }

    learn_N = *(U16*)data;

    printf("%02x%02x\n", data[0], data[1]);

    //
    // Get learn string data
    //

    gpib_lock();
    data = (uint8_t *)GPIB_read_BIN(len, TRUE, FALSE, &learn_string_size);

    gpib_unlock();
    log_session("b:", "...");

    if (learn_string_size != len)
    {
        printf("Error: OUTPLEAS string len = %d, received %d", len, learn_string_size);
        return;
    }

    for (int i = 0; i < learn_string_size; i++)
    {
        printf("%02x", data[i]);
        if ((i % 40 == 39) && (i < learn_string_size - 1)) printf("\n");
    }
    printf("\n");
    fflush(stdout);

    Sleep(500);
    gpib_lock();
    log_session("p>", "DEBUOFF;CONT;");
    gpib_unlock();
    GPIB_printf("DEBUOFF;CONT;");
}

void setcalib()
{
    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }
   
    char active_cal_name[20];
    int n_arrays;
    scanf("%20s\n", active_cal_name);

    char str[40];
    sprintf(str, "CORROFF;FORM1;%s;", active_cal_name);
    log_session("p>", str);

    gpib_lock();
    GPIB_printf(str);
    gpib_unlock();

    scanf("%d", &n_arrays);
    for (int i = 0; i < n_arrays; i++)
    {
        int array_bytes, val;

        scanf("\n%d\n", &array_bytes);
        uint8_t* data = (uint8_t*)malloc(15 + array_bytes);

        sprintf((char*)data, "INPUCALC%d%d;", (i + 1) / 10, (i + 1) % 10);
        data[11] = '#';
        data[12] = 'A';
        data[13] = (array_bytes >> 8);
        data[14] = (array_bytes & 255);

        for (int i = 0; i < array_bytes; i++)
        {
            scanf("%02x", &val);
            data[15 + i] = (uint8_t)val;
            if ((i % 40 == 39) && (i < array_bytes - 1)) scanf("\n");
        }
        
        log_session("b>", "...");
        gpib_lock();
        GPIB_write_BIN(data, 15 + array_bytes);
        gpib_unlock();

        Sleep(250);

        log_session("p>", "CLES;SRE 32;ESE 1;OPC;SAVC;");

        gpib_lock();
        GPIB_printf("CLES;SRE 32;ESE 1;OPC;SAVC;");
        gpib_unlock();

        S32 st = timeGetTime();
        while (1)
        {
            Sleep(100);
            gpib_lock();
            U8 result = GPIB_serial_poll();
            gpib_unlock();

            if (result & 0x40)
            {
                printf("!Complete in %d ms\n", timeGetTime() - st);
                break;
            }
        }
        log_session("p>", "CLES;SRE 0;");

        gpib_lock();
        GPIB_printf("CLES;SRE 0;");
        gpib_unlock();

        free(data);
    }
    Sleep(1500);
    log_session("p>", "DEBUOFF;CONT;");
    gpib_lock();
    GPIB_printf("DEBUOFF;CONT;");
    gpib_unlock();
}

void setstate()
{
    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }
    int val, val2;
    uint8_t header[2], len[2];

    scanf("%02x%02x", &val, &val2);
    header[0] = (uint8_t)val;
    header[1] = (uint8_t)val2;
    if ((header[0] != '#') || (header[1] != 'A'))
    {
        printf("!setstate: header does not match\n");
        fflush(stdout);
        return;
    }
    scanf("%02x%02x\n", &val, &val2);
    len[0] = (uint8_t)val;
    len[1] = (uint8_t)val2;
    uint16_t length = (len[0] << 8) + len[1];  // 16-bit, big endian
    uint8_t *data = (uint8_t *)malloc(22 + length);
    sprintf((char *)data, "FORM1;INPULEAS;");
    data[15] = header[0];
    data[16] = header[1];
    data[17] = len[0];
    data[18] = len[1];
    for (int i = 0; i < length; i++)
    {        
        if (scanf("%02x", &val) != 1)
        {
            printf("!setstate: state data broken\n");
            fflush(stdout);
            return;
        }
        data[i + 19] = (uint8_t)val;
        //if ((i % 40 == 39) && (i < length - 1)) scanf("\n");
    }
    //scanf("\n");
    //printf("!going to send...\n");
        
    log_session("b>", "...");
    gpib_lock();
    GPIB_write_BIN(data, length + 19);
    gpib_unlock();
    Sleep(1500);

    log_session("p>", "DEBUOFF;CONT;");
    gpib_lock();
    GPIB_printf("DEBUOFF;CONT;");
    gpib_unlock();

    free(data);
}

void reset()
{
    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }

    log_session("p>", "DEBUOFF;CONT;");
    gpib_lock();
    GPIB_puts("DEBUOFF;CONT;");
    gpib_unlock();
}

void factory_reset()
{
    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }
    if (!is_8510())
    {
        log_session("p>", "RST;");
        gpib_lock();
        GPIB_puts("RST;");
        gpib_unlock();
    }
    else
    {
        if (is_8510C())
        {
            log_session("p>", "FACTPRES;");
            gpib_lock();
            GPIB_puts("FACTPRES;");
            gpib_unlock();
        }
        else
        {
            log_session("p>", "PRES;");
            gpib_lock();
            GPIB_puts("PRES;");
            gpib_unlock();
        }
    }
}

void direct_command(action_type requested_action, const char *action_argument)
{
    // s string   - send string using gpib_puts()
    // q query    - send string using gpib_query() and print result
    // a          - read and print ascii response
    // c          - read and print ascii response until next action
    // d          - read and print ascii response N-times
    // b          - read and print binary response
    // ?          - print status
    // .          - exit direct command mode
    uint8_t* data;
    int len;
    char* response = 0;

    if (!connected)
    {
        printf("!not connected\n");
        fflush(stdout);
        return;
    }

    new_input_entered = 0;

    switch (requested_action)
    {
    case action_cmd_puts:
        //DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD
	    log_session("s>", action_argument);
        gpib_lock();
        GPIB_puts(action_argument);
        gpib_unlock();
        break;
    case action_cmd_query:
        log_session("q>", action_argument);
        gpib_lock();
        response = GPIB_query(action_argument);
        gpib_unlock();
        log_session("q:", response);
        printf("%s", response);
        fflush(stdout);
        break;
    case action_cmd_read_asc:
        gpib_lock();
        data = (uint8_t *)GPIB_read_ASC(); 
        gpib_unlock();
        log_session("a:", (const char*)data);
        printf("%s", data);
        fflush(stdout);
        break;
    case action_cmd_continuous_asc:
        do {
            gpib_lock();
            data = (uint8_t*)GPIB_read_ASC();
            gpib_unlock();
            if (strlen((char *)data) > 0)
            {
                printf("%s", data);
                fflush(stdout);
                log_session("c:", (const char *)data);
            }
            else Sleep(1);
        } while (!new_input_entered);
        break;
    case action_cmd_repeated_asc:
        sscanf(action_argument, "%d", &cmd_read_repeat_count);

        for (int i = 0; i < cmd_read_repeat_count; i++)
        {
            gpib_lock();
            data = (uint8_t*)GPIB_read_ASC();
            gpib_unlock();
            if (strlen((char*)data) > 0)
            {
                printf("%s", data);
                fflush(stdout);
                log_session("d:", (const char*)data);
            }
            else Sleep(1);
        } 
        break;
    case action_cmd_read_bin:
        gpib_lock();
        data = (uint8_t*)GPIB_read_BIN();
        gpib_unlock();
        log_session("b:", "...");
        if ((data[0] != '#') || (data[1] != 'A'))
        {
            printf("!header not received\n");
            fflush(stdout);
            break;
        }
        len = (data[2] << 8) + data[3];
        for (int i = 0; i < len + 4; i++)
            printf("%02x", data[i]);
        printf("\n");
        fflush(stdout);
        break;
    case action_cmd_status:
        gpib_lock();
        S32 status = GPIB_status();            
        gpib_unlock();
        printf("%d\n", status);
        fflush(stdout);
        char status_str[20];
        sprintf(status_str, "%d", status);
        log_session("?:", status_str);
        break;
    } 
}

int measure()
{
    if (connect())
        if (sweep())
        {
            disconnect();
            return 1;
        }
    return 0;
}

void help()
{
    printf("         CONNECT    ... connect to the device\n");
    printf("         DISCONNECT ... disconnect the device\n");
    printf("         S11 .. S22 ... configure (add) a channel for measurement\n");
    printf("         ALL        ... configure measurement of all 4 channels\n");
    printf("         CLEAR      ... reset measurement config to no channels\n");
    printf("         FORM1|FORM4 ... setup sending format 1=fast, 4=ascii\n");
    printf("         FMT RI|MA|DB ... configure measure data format (RI default)\n");
    printf("         FREQ HZ|KHZ|MHZ|GHZ ... configure freq format (GHZ default)\n");
    printf("         MEASURE    ... perform configured measurement\n");
    printf("         M+         ... perform repeated configured measurements\n");
    printf("         M-         ... stop the repetitions of the measurements\n");
    printf("         FILE path  ... configure file to save the next measurement\n");
    printf("                        for continuous, they are prefixed with XXXX_\n");
    printf("         GETSTATE   ... dump the device state\n");
    printf("         SETSTATE   ... set the device state (followed in next line)\n");
    printf("         GETCALIB   ... get the device calibration\n");
    printf("         SETCALIB   ... set the device calibration (followed in next line)\n");
    printf("         RESET      ... reset instrument\n");
    printf("         FACTRESET  ... factory reset instrument\n");
    printf("         CMD        ... enter direct command mode:\n");
    printf("             s str  ... send the string using gpib_puts()\n");
    printf("             q str  ... send a query and read a string\n");
    printf("                        response using gpib_query()\n");
    printf("             a      ... retrieve response with gpib_read_ASC()\n");
    printf("             c      ... continuous gpib_read_ASC() until next input\n");
    printf("             d n    ... continuous gpib_read_ASC() N-times\n");
    printf("             b      ... retrieve response with gpib_read_BIN()\n");
    printf("             ?      ... read and print status\n");
    printf("             .      ... leave direct command mode\n");
    printf("         HELP       ... print this help\n");
    printf("         LOGON      ... turn on session logging to log_session.txt\n");
    printf("         LOGOFF     ... turn off session logging (default)\n");
    printf("         EXIT       ... terminate the application\n");
    fflush(stdout);
}

void print_usage()
{
    printf("usage: hpctrl [-a n] [-i | [-S11][-S21][-S12][-S22]]\n\n");
    printf(" -a n   specify device address, default=16\n");
    printf(" -Sxy   retrieve measurement from channel xy\n");
    printf(" -i     interactive mode, accepted commands:\n");
    help();
}

void parse_cmdline(int argc, const char** argv)
{
    for (int i = 1; i < argc; i++)
        if (_stricmp(argv[i], "-i") == 0) cmdline_i = 1;
        else if (_stricmp(argv[i], "-a") == 0) { if (++i < argc) sscanf(argv[i], "%d", &cmdline_a); }
        else if (_stricmp(argv[i], "-s11") == 0) cmdline_s11 = 1;
        else if (_stricmp(argv[i], "-s21") == 0) cmdline_s21 = 1;
        else if (_stricmp(argv[i], "-s12") == 0) cmdline_s12 = 1;
        else if (_stricmp(argv[i], "-s22") == 0) cmdline_s22 = 1;
}

void set_file()
{
    if (strlen(ln) < 6) return;
    if (save_file_name) free(save_file_name);
    save_file_name = (char*)malloc(strlen(ln + 5) + 1);
    strcpy(save_file_name, ln + 5);
}

void set_format(char* ln)
{
    if (strlen(ln + 3) < 3) return;
    ln += 4;
    if (_stricmp(ln, "RI") == 0) current_format = fmt_ri;
    else if (_stricmp(ln, "MA") == 0) current_format = fmt_ma;
    else if (_stricmp(ln, "DB") == 0) current_format = fmt_db;
}

void set_freq_format(char* ln)
{
    if (strlen(ln + 4) < 3) return;
    ln += 5;
    if (_stricmp(ln, "HZ") == 0) current_freq_format = hz;
    else if (_stricmp(ln, "KHZ") == 0) current_freq_format = khz;
    else if (_stricmp(ln, "MHZ") == 0) current_freq_format = mhz;
    else if (_stricmp(ln, "GHZ") == 0) current_freq_format = ghz;
}

void set_sending_format(char f)
{
    if (f == '1') current_sending_format = form1;
    else if (f == '4') current_sending_format = form4;
    else
    {
        printf("!unsupported format %c", f);
        fflush(stdout);
    }
}

void perform_action(action_type action, char *action_argument)
{
    switch (action)
    {
    case action_connect: if (action_argument) sscanf(action_argument, "%d", &cmdline_a);
                         connect(); 
                         break;
    case action_disconnect: disconnect(); break;
    case action_s11: cmdline_s11 = 1; break;
    case action_s12: cmdline_s12 = 1; break;
    case action_s21: cmdline_s21 = 1; break;
    case action_s22: cmdline_s22 = 1; break;
    case action_all: cmdline_s11 = cmdline_s21 = cmdline_s12 = cmdline_s22 = 1; break;
    case action_clear: cmdline_s11 = cmdline_s21 = cmdline_s12 = cmdline_s22 = 0; break;
    case action_form: set_sending_format(action_argument[0]); break;
    case action_fmt: set_format(action_argument); break;
    case action_freq: set_freq_format(action_argument); break;
    case action_file: set_file(); break;
    case action_sweep: sweep(); break;
    case action_autosweep: autosweep = 1; sweep(); break;
    case action_getstate: getstate(); break;
    case action_setstate: setstate(); current_input_mode = mode_menu; break;
    case action_getcalib: getcalib(); break;
    case action_setcalib: setcalib(); current_input_mode = mode_menu; break;
    case action_cmd_puts:
    case action_cmd_status:
    case action_cmd_query:
    case action_cmd_read_asc:
    case action_cmd_continuous_asc:
    case action_cmd_repeated_asc:
    case action_cmd_read_bin:
        direct_command(action, action_argument);
        break;
    case action_reset: reset(); break;
    case action_freset: factory_reset(); break;
    }
    if (action_argument) free(action_argument);
}

// mutex already locked when dequeue_action called
action_type dequeue_action(char** action_argument)
{
    *action_argument = 0;
    if (aq_rp == aq_wp) return action_none;
    action_type at = action_queue[aq_rp];
    *action_argument = action_arguments[aq_rp];
    aq_rp = (aq_rp + 1) % ACTION_QUEUE_SIZE;
    return at;
}

void enqueue_action(action_type action, char* action_argument)
{
    char* aa = 0;
    if (action_argument)
    {
        aa = (char*)malloc(strlen(action_argument) + 1);
        strcpy(aa, action_argument);
    }

    DWORD dwWaitResult = WaitForSingleObject(aq_lock, INFINITE);
    if (dwWaitResult != WAIT_OBJECT_0) return;

    action_queue[aq_wp] = action;
    action_arguments[aq_wp] = aa;
    if (aq_wp == aq_rp) 
      if (!SetEvent(action_event))
      {
          printf("SetEvent failed (%d)\n", GetLastError());
          exit(1);
      }
    aq_wp = (aq_wp + 1) % ACTION_QUEUE_SIZE;
    if (aq_wp == aq_rp)
    {
        printf("The capacity of action queue (%d) was exahusted\n", ACTION_QUEUE_SIZE);
        exit(1);
    }
    ReleaseMutex(aq_lock);
}

void parse_end_enqueue_cmd_action(char* ln)
{
    if (ln[0] == '.') current_input_mode = mode_menu;
    else if ((ln[0] == 's') && (strlen(ln) > 2)) enqueue_action(action_cmd_puts, ln + 2);
    else if ((ln[0] == 'q') && (strlen(ln) > 2)) enqueue_action(action_cmd_query, ln + 2);
    else if (ln[0] == 'a') enqueue_action(action_cmd_read_asc, ln + 2);
    else if (ln[0] == 'c') enqueue_action(action_cmd_continuous_asc, ln + 2);
    else if (ln[0] == 'd') enqueue_action(action_cmd_repeated_asc, ln + 2);
    else if (ln[0] == 'b') enqueue_action(action_cmd_read_bin, 0); 
    else if (ln[0] == '?') enqueue_action(action_cmd_status, 0);
}

void parse_end_enqueue_menu_action(char* ln)
{
    if (_strnicmp(ln, "CONNECT", 7) == 0)
    {
        if ((strlen(ln) > 7) && (ln[8] >= '0') && (ln[8] <= '9'))
            enqueue_action(action_connect, ln + 8);
        else enqueue_action(action_connect, 0);
    }
    else if (_stricmp(ln, "DISCONNECT") == 0) enqueue_action(action_disconnect, 0);
    else if (_stricmp(ln, "S11") == 0) enqueue_action(action_s11, 0);
    else if (_stricmp(ln, "S21") == 0) enqueue_action(action_s21, 0);
    else if (_stricmp(ln, "S12") == 0) enqueue_action(action_s12, 0);
    else if (_stricmp(ln, "S22") == 0) enqueue_action(action_s22, 0);
    else if (_stricmp(ln, "ALL") == 0) enqueue_action(action_all, 0);
    else if (_stricmp(ln, "CLEAR") == 0) enqueue_action(action_clear, 0);
    else if (_strnicmp(ln, "FORM", 4) == 0) enqueue_action(action_form, ln + 4);
    else if (_strnicmp(ln, "FMT", 3) == 0) enqueue_action(action_fmt, ln);
    else if (_strnicmp(ln, "FREQ", 4) == 0) enqueue_action(action_freq, ln);
    else if (_strnicmp(ln, "FILE", 4) == 0) enqueue_action(action_file, ln);
    else if (_stricmp(ln, "MEASURE") == 0) enqueue_action(action_sweep, 0);
    else if (_stricmp(ln, "M+") == 0) enqueue_action(action_autosweep, 0);
    else if (_stricmp(ln, "GETSTATE") == 0) enqueue_action(action_getstate, 0);
    else if (_stricmp(ln, "SETSTATE") == 0)
    {
        current_input_mode = mode_input_blocked;
        enqueue_action(action_setstate, 0);
    }
    else if (_stricmp(ln, "GETCALIB") == 0) enqueue_action(action_getcalib, 0);
    else if (_stricmp(ln, "SETCALIB") == 0)
    {
        current_input_mode = mode_input_blocked;
        enqueue_action(action_setcalib, 0);
    }
    else if (_stricmp(ln, "RESET") == 0) enqueue_action(action_reset, 0);
    else if (_stricmp(ln, "FACTRESET") == 0) enqueue_action(action_freset, 0);
    else if (_stricmp(ln, "CMD") == 0) current_input_mode = mode_cmd;
    else printf("!unknown command %s\n", ln);
    fflush(stdout);
}

DWORD WINAPI interactive_thread(LPVOID arg)
{
    action = action_none;

    do {
        while (current_input_mode == mode_input_blocked)
            Sleep(1);

        fgets(ln, 50, stdin);
        while ((ln[strlen(ln) -1] == 13) || (ln[strlen(ln) - 1] == 10)) 
            ln[strlen(ln) - 1] = 0; //strip CR,LF
        if (strlen(ln) == 0) continue;
        new_input_entered = 1;

        log_session("!>", ln);

        if (_stricmp(ln, "HELP") == 0) help();
        else if (_stricmp(ln, "USAGE") == 0) print_usage();
        else if (_stricmp(ln, "EXIT") == 0) { running = 0; break; }
        else if (_stricmp(ln, "LOGON") == 0) session_logging = 1; 
        else if (_stricmp(ln, "LOGOFF") == 0) session_logging = 0; 
        else if (_stricmp(ln, "M-") == 0) autosweep = 0;
        else if (current_input_mode == mode_menu)
            parse_end_enqueue_menu_action(ln);
        else if (current_input_mode == mode_cmd)
            parse_end_enqueue_cmd_action(ln);
    } while (running);

    if (connected)
    {
        printf("!auto disconnect before exit\n");
        fflush(stdout);
        if (gpib_trylock()) disconnect();
    }
    exit(0);
}

void create_event_and_thread()
{
    aq_wp = 0;
    aq_rp = 0;

    action_event = CreateEvent(
        NULL,               // default security attributes
        FALSE,               // auto-reset event
        FALSE,              // initial state is nonsignaled
        0                   // no event object name
    );

    if (action_event == NULL)
    {
        printf("CreateEvent failed (%d)\n", GetLastError());
        fflush(stdout);
        exit(1);
    }

    aq_lock = CreateMutex(
        NULL,           // default security attributes
        FALSE,          // initially not owned
        NULL            // unnamed mutex
    );

    if (aq_lock == NULL)
    {
        printf("CreateMutex failed (%d)\n", GetLastError());
        fflush(stdout);
        exit(1);
    }

    gpib_mutex = CreateMutex(
        NULL,              // default security attributes
        FALSE,             // initially not owned
        NULL);             // unnamed mutex

    if (gpib_lock == NULL)
    {
        printf("CreateMutex error: %d\n", GetLastError());
        fflush(stdout);
        exit(1);
    }

    HANDLE t = CreateThread(
        NULL,                   // default security attributes
        0,                      // use default stack size  
        interactive_thread,       // thread function name
        0,                       // argument to thread function 
        0,                      // use default creation flags 
        0);                    // does not return the thread identifier 

    if (t == NULL)
    {
        printf("CreateThread failed (%d)\n", GetLastError());
        fflush(stdout);
        exit(1);
    }
}

void main_action_loop()
{
    while (running)
    {
        DWORD dwWaitResult = WaitForSingleObject(aq_lock, INFINITE);
        if (dwWaitResult != WAIT_OBJECT_0) 
        {
            printf("Wait error (%d)\n", GetLastError());
            fflush(stdout);
            exit(1);
        }

        if (aq_rp == aq_wp)
        {
            ReleaseMutex(aq_lock);
            DWORD event = WaitForSingleObject(
                action_event, // event handle
                INFINITE);    // indefinite wait

            if (event != WAIT_OBJECT_0)
            {
                printf("Wait error (%d)\n", GetLastError());
                fflush(stdout);
                exit(1);
            }
            DWORD dwWaitResult = WaitForSingleObject(aq_lock, INFINITE);
            if (dwWaitResult != WAIT_OBJECT_0)
            {
                printf("Wait error (%d)\n", GetLastError());
                fflush(stdout);
                exit(1);
            }
        }        

        char *aa;
        action_type requested_action = dequeue_action(&aa);
        ReleaseMutex(aq_lock);

        if (requested_action != action_none)
            perform_action(requested_action, aa);
    }
}

void interactive()
{
    create_event_and_thread();
    main_action_loop();
    CloseHandle(action_event);
    CloseHandle(aq_lock);
    CloseHandle(gpib_mutex);
}

const char* test1argv[] = { "hpctrl", "-a", "16", "-s11", "-s12", "-s21", "-s22" };
const char* test2argv[] = { "hpctrl", "-a", "19", "-i" };

int test1argc = 7;
int test2argc = 4;

//int runtest = 2;
int runtest = 0;

int main(int argc, char** argv)
{
    const char** av = (const char**)argv;

    if (runtest == 1)
    {
        av = test1argv;
        argc = test1argc;
    }
    else if (runtest == 2)
    {
        av = test2argv;
        argc = test2argc;
    }
    
    if (argc < 2)
        print_usage();
    else
    {
        parse_cmdline(argc, av);
        if (cmdline_i) interactive();
        else measure();
    }
}
