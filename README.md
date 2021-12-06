# hpctrl
A console application to control HP4191A, HP8753 and HP83480A over GPIB and NI cards

It is based on the work of John Miles, KE5FX, john@miles.io  
GPIB Toolkit, version 1.981 from March 26 2020, see http://www.ke5fx.com/gpib/readme.htm  

It was developed for research work at Faculty of Mathematics, Physics and Informatics,  
in particular as a support for student projects:  
  https://github.com/TIS2020-FMFI/plazma  (HP8753)  
  https://github.com/TIS2020-FMFI/hp (HP4191A)  
  https://github.com/TIS2021-FMFI/osciloskop (HP83480A)

It compiles with MSVS 2019 Community version. We use Win32 Debug mode.  

For HP4191A, only the "cmd" mode in interactive mode is useful. 
Commands i,8,16,*,1234 are useful only for HP83480A. 
Bugs, comments, ideas: Pavel Petrovic, pavel.petrovic at gmail.com.  

Run debug/hpctrl.exe to see the usage:  
 	
~~~~
usage: hpctrl [-a n] [-i | [-S11][-S21][-S12][-S22]]  
  
 -a n   specify device address, default=16  
 -Sxy   retrieve measurement from channel xy  
 -i     interactive mode, accepted commands:  
         CONNECT    ... connect to the device
         DISCONNECT ... disconnect the device
         OSCI       ... disable instrument setup (do it for 83480A & similar)
         S11 .. S22 ... configure (add) a channel for measurement
         ALL        ... configure measurement of all 4 channels
         CLEAR      ... reset measurement config to no channels
         FORM1|FORM4 ... setup sending format 1=fast, 4=ascii
         FMT RI|MA|DB ... configure measure data format (RI default)
         FREQ HZ|KHZ|MHZ|GHZ ... configure freq format (GHZ default)
         MEASURE    ... perform configured measurement
         M+         ... perform repeated configured measurements
         M-         ... stop the repetitions of the measurements
         FILE path  ... configure file to save the next measurement
                        for continuous, they are prefixed with XXXX_
         GETSTATE   ... dump the device state
         SETSTATE   ... set the device state (followed in next line)
         GETCALIB   ... get the device calibration
         SETCALIB   ... set the device calibration (followed in next line)
         RESET      ... reset instrument
         FACTRESET  ... factory reset instrument
         CMD        ... enter direct command mode:
             s str  ... send the string using gpib_puts()
             q str  ... send a query and read a string
                        response using gpib_query()
             a      ... retrieve response with gpib_read_ASC()
             c      ... continuous gpib_read_ASC() until next input
             d n    ... continuous gpib_read_ASC() N-times
             b      ... retrieve response with gpib_read_BIN()
             ?      ... read and print status
             i      ... read any binary oscilloscope format, print in hex\n");
             8      ... read BYTE format from oscilloscope and print\n");
             16     ... read WORD format from oscilloscope and print\n");
             *      ... continuous read WORD format from oscilloscope\n");
                        result is saved to file configured with FILE\n");
             1234   ... select channels to read from, specify 1-4 channels\n");			 
             .      ... leave direct command mode
         HELP       ... print this help
         LOGON      ... turn on session logging to log_session.txt
         LOGOFF     ... turn off session logging (default)
         EXIT       ... terminate the application
~~~~
