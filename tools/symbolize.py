"""Names for offsets into the mod's DLL (as tools/crashinfo.py prints them), from the build's PDB, through Windows'
dbghelp.

    python tools/symbolize.py <dll> 0x4a473 0x47ac5 ...
"""
import ctypes
import os
import sys
from ctypes import wintypes

BASE = 0x10000000


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_ulonglong * 2),
                ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_ulonglong),
                ("Flags", wintypes.ULONG), ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong),
                ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", wintypes.WCHAR * 512)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wintypes.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_ulonglong)]


def main(dll, offsets):
    dbghelp = ctypes.WinDLL("dbghelp.dll")
    process = ctypes.windll.kernel32.GetCurrentProcess()
    dbghelp.SymSetOptions(0x2 | 0x10)  # undecorated names, line numbers
    dbghelp.SymInitializeW(process, os.path.dirname(os.path.abspath(dll)), False)
    dbghelp.SymLoadModuleExW.restype = ctypes.c_ulonglong
    if not dbghelp.SymLoadModuleExW(process, None, os.path.abspath(dll), None, ctypes.c_ulonglong(BASE), 0, None, 0):
        print("couldn't load", dll)
        return
    for text in offsets:
        address = BASE + int(text, 16)
        info = SYMBOL_INFOW()
        info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW) - ctypes.sizeof(wintypes.WCHAR) * 511
        info.MaxNameLen = 512
        displacement = ctypes.c_ulonglong(0)
        name = "?"
        if dbghelp.SymFromAddrW(process, ctypes.c_ulonglong(address), ctypes.byref(displacement), ctypes.byref(info)):
            name = f"{info.Name}+0x{displacement.value:x}"
        line = IMAGEHLP_LINEW64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINEW64)
        d32 = wintypes.DWORD(0)
        where = ""
        if dbghelp.SymGetLineFromAddrW64(process, ctypes.c_ulonglong(address), ctypes.byref(d32), ctypes.byref(line)):
            where = f"  {os.path.basename(line.FileName)}:{line.LineNumber}"
        print(f"{text}: {name}{where}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
