TARGET_OS ?= $(shell uname)
ifeq ($(TARGET_OS),)
  TARGET_OS ?= $(OS)
endif

ifneq (,$(filter Windows%,$(TARGET_OS)))
LIBLZ4 = liblz4-$(LIBVER_MAJOR)
LIBLZ4_EXP = liblz4.lib
WINBASED   = yes
else
LIBLZ4_EXP = liblz4.dll.a
  ifneq (,$(filter MINGW%,$(TARGET_OS)))
LIBLZ4     = liblz4
WINBASED   = yes
  else
    ifneq (,$(filter MSYS%,$(TARGET_OS)))
LIBLZ4     = msys-lz4-$(LIBVER_MAJOR)
WINBASED   = yes
    else
      ifneq (,$(filter CYGWIN%,$(TARGET_OS)))
LIBLZ4     = cyglz4-$(LIBVER_MAJOR)
WINBASED   = yes
      else
LIBLZ4     = liblz4.$(SHARED_EXT_VER)
WINBASED   = no
EXT        =
      endif
    endif
  endif
endif

ifeq ($(WINBASED),yes)
EXT        = .exe
endif

#determine if dev/nul based on host environment
ifneq (,$(filter MINGW%,$(shell uname)))
VOID := /dev/null
else
  ifneq (,$(filter MSYS%,$(shell uname)))
VOID := /dev/null
  else
    ifneq (,$(filter CYGWIN%,$(shell uname)))
VOID := /dev/null
    else
      ifneq (,$(filter Windows%,$(OS)))
VOID := nul
      else
VOID  := /dev/null
      endif
    endif
  endif
endif

ifneq (,$(filter Linux Darwin GNU/kFreeBSD GNU OpenBSD FreeBSD NetBSD DragonFly SunOS Haiku MidnightBSD MINGW32% MINGW64% CYGWIN% MSYS%,$(shell uname)))
POSIX_ENV = Yes
else
POSIX_ENV = No
endif
