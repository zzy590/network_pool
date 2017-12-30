# network_pool
A async network connection pool written in C++ with libuv.

# Benchmark
ApacheBench of a simple http server with network_pool:

Single thread on single core 1GB ram of Tencent Cloud Virtual Machine.
Memory usage(by CmemoryTrace) is about 30MB(peak), <1MB(most time).

cat /proc/cpuinfo
processor       : 0
vendor_id       : GenuineIntel
cpu family      : 6
model           : 63
model name      : Intel(R) Xeon(R) CPU E5-26xx v3
stepping        : 2
microcode       : 0x1
cpu MHz         : 2299.996
cache size      : 4096 KB
physical id     : 0
siblings        : 1
core id         : 0
cpu cores       : 1
apicid          : 0
initial apicid  : 0
fpu             : yes
fpu_exception   : yes
cpuid level     : 13
wp              : yes
flags           : fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx lm constant_tsc rep_good nopl eagerfpu pni pclmulqdq ssse3 fma cx16 pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand hypervisor lahf_lm abm bmi1 avx2 bmi2 xsaveopt
bogomips        : 4599.99
clflush size    : 64
cache_alignment : 64
address sizes   : 40 bits physical, 48 bits virtual
power management:

./ab -c 10000 -n 200000 http://127.0.0.1/index.html
This is ApacheBench, Version 2.3 <$Revision: 1430300 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient)
Completed 20000 requests
Completed 40000 requests
Completed 60000 requests
Completed 80000 requests
Completed 100000 requests
Completed 120000 requests
Completed 140000 requests
Completed 160000 requests
Completed 180000 requests
Completed 200000 requests
Finished 200000 requests


Server Software:
Server Hostname:        127.0.0.1
Server Port:            80

Document Path:          /index.html
Document Length:        5 bytes

Concurrency Level:      10000
Time taken for tests:   12.227 seconds
Complete requests:      200000
Failed requests:        0
Write errors:           0
Total transferred:      8600000 bytes
HTML transferred:       1000000 bytes
Requests per second:    16357.69 [#/sec] (mean)
Time per request:       611.333 [ms] (mean)
Time per request:       0.061 [ms] (mean, across all concurrent requests)
Transfer rate:          686.90 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0  474 1099.7      3    7031
Processing:     2   27 103.9      6    1649
Waiting:        0   24 103.4      4    1646
Total:          4  501 1133.3     10    7850

Percentage of the requests served within a certain time (ms)
  50%     10
  66%     14
  75%   1010
  80%   1015
  90%   1207
  95%   3020
  98%   3421
  99%   7036
 100%   7850 (longest request)
