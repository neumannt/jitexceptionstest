C++ exception experiment with JIT-ed code
-----------------------------------------

This project tests the performance of C++
exception handling when JIT code is generated
at runtime. The traditional libgcc does not
scale due to a global lock that is used when
JIT code has been registered, which results
in the following performance numbers on
a dual-socket EPYC 7713:

**thread count**|**1**|**2**|**4**|**8**|**16**|**32**|**64**|**128**
:-----:|:-----:|:-----:|:-----:|:-----:|:-----:|:-----:|:-----:|:-----:
failure rate 0%:|12|20|20|20|24|43|91|179
failure rate 0.1%:|17|18|17|21|19|35|74|136
failure rate 1%:|18|20|21|24|41|122|461|1923
failure rate 10%:|29|42|70|144|381|994|4670|1889

Unwinding performance is extremely poor, we get
a slow-down of xmore than 60 if exceptions are common.
That that even without exceptions (failure rate 0%)
we get some slow-down here, as the threads are constantly
creating and discard JIT code, which does not scale
perfectly either. We do that to stress-test the
mechanism for registering JIT code.

The performance problems within the unwinder can be
eliminated using a
[libgcc patch][https://gcc.gnu.org/pipermail/gcc-patches/2022-March/591203.html]
which will hopefully make into into gcc at some point.

