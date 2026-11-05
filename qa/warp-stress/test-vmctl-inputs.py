#!/usr/bin/env python3
"""Verify the actual VMCTL input functions with a non-PM recording sink."""
from pathlib import Path
import re
import subprocess
import sys

suite = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv) > 1 else suite / "build/warp-stress").resolve()
build.mkdir(parents=True, exist_ok=True)
source = (suite / "qa/warp-stress/vmctl.c").read_text()
functions = source[source.index("static int post_key("):source.index("static int number(")]
header = Path(sys.argv[2]) if len(sys.argv) > 2 else suite.parent / "toolchain/openwatcom/h/os2/pmwin.h"
definitions = []
for name, value in re.findall(r"^#define\s+((?:KC_|VK_|WM_CHAR)[A-Z0-9_]*)\s+(0x[0-9a-fA-F]+)\b", header.read_text(), re.M):
    definitions.append(f"#define {name} {value}")
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
typedef uint16_t USHORT;
typedef uint32_t HWND;
typedef uint32_t MPARAM;
#define stricmp strcasecmp
#define MPFROMSH2CH(s,a,b) ((uint32_t)(s)|((uint32_t)(a)<<16)|((uint32_t)(b)<<24))
#define MPFROM2SHORT(a,b) ((uint32_t)(a)|((uint32_t)(b)<<16))
struct Message { HWND window;uint32_t kind,first,second; } messages[64];
static unsigned count,fail;
static int WinPostMsg(HWND w,uint32_t k,MPARAM a,MPARAM b) {
 if(fail)return 0;
 assert(count<64);messages[count].window=w;messages[count].kind=k;
 messages[count].first=a;messages[count].second=b;count++;return 1;
}
static void DosSleep(unsigned delay) { (void)delay; }
'''
checks = r'''
static void check(const char *description,uint16_t flags,uint16_t ch,uint16_t vk) {
 count=0;assert(key(123,description));assert(count==2);
 assert(messages[0].window==123&&messages[0].kind==WM_CHAR);
 assert(messages[0].first==MPFROMSH2CH(flags,1,0));
 assert(messages[1].first==MPFROMSH2CH(flags|KC_KEYUP,1,0));
 assert(messages[0].second==MPFROM2SHORT(ch,vk));assert(messages[1].second==messages[0].second);
}
int main(void) {
 check("ctrl-o",KC_CTRL|KC_CHAR,'o',0);
 check("CTRL-S",KC_CTRL|KC_CHAR,'S',0);
 check("ctrl-shift-f12",KC_CTRL|KC_SHIFT|KC_VIRTUALKEY,0,VK_F12);
 check("alt-f4",KC_ALT|KC_VIRTUALKEY,0,VK_F4);
 check("shift-tab",KC_SHIFT|KC_VIRTUALKEY,0,VK_TAB);
 check("ctrl-home",KC_CTRL|KC_VIRTUALKEY,0,VK_HOME);
 check("enter",KC_VIRTUALKEY,0,VK_ENTER);
 check("escape",KC_VIRTUALKEY,0,VK_ESC);
 check("space",KC_CHAR,' ',0);
 count=0;assert(!key(123,"ctrl"));assert(!key(123,"f13"));assert(!key(123,"f1junk"));
 assert(!key(123,"enter-tab"));assert(count==0);
 fail=1;assert(!key(123,"ctrl-o"));assert(!text(123,"a"));assert(count==0);fail=0;
 count=0;assert(text(456,"Az\t\n"));assert(count==8);
 assert(messages[0].second=='A'&&messages[2].second=='z');
 assert(messages[4].second==MPFROM2SHORT(0,VK_TAB));assert(messages[6].second==MPFROM2SHORT(0,VK_ENTER));
 puts("PASS actual VMCTL key/text packet flags, press/release, invalid-key rejection and post failures");return 0;
}
'''
unit = build / "vmctl-inputs.c"
unit.write_text(program + "\n".join(definitions) + "\n" + functions + checks)
binary = build / "vmctl-inputs"
subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
