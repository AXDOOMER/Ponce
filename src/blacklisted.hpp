//! \file
/*
**  Copyright (c) 2016 - Ponce
**  Authors:
**         Alberto Garcia Illera        agarciaillera@gmail.com
**         Francisco Oca                francisco.oca.gonzalez@gmail.com
**
**  This program is under the terms of the BSD License.
*/

#pragma once

std::vector<std::string> black_func = {
	"printf",
	"puts",
	"putc",
	"sleep",
	"recv",
	"recvfrom",
	"send",
	"closesocket",
	"exit",
	"abort",
	"malloc",
	"calloc",
	"realloc",
	"free",
	"calloc_crt",
	"realloc_crt",
	
	//cstdio
	"clearerr",
	"fclose",
	"feof",
	"ferror",
	"fflush",
	"fgetc",
	"fgetpos",
	"fgets",
	"fopen",
	"fprintf",
	"fputc",
	"fputs",
	"fread",
	"freopen",
	"fscanf",
	"fseek",
	"fsetpos",
	"ftell",
	"fwrite",
	"getc",
	"getchar",
	"gets",
	"perror",
	"printf",
	"putc",
	"putchar",
	"puts",
	"remove",
	"rename",
	"rewind",
	"scanf",
	"setbuf",
	"setvbuf",
	"snprintf",
	"sprintf",
	"sscanf",
	"tmpfile",
	"tmpnam",
	"ungetc",
	"vfprintf",
	"vfscanf",
	"vprintf",
	"vscanf",
	"vsnprintf",
	"vsprintf",
	"vsscanf",

	//Windows
	"Sleep",
	"HeapAlloc",
	"HeapFree",
	"HeapRealloc",
	"CreateThread",
	"NtTerminateProcess",
	"ExitThread",
	"TerminateThread",
	"ExitProcess",

	"EnterCriticalSection", 
	"LeaveCriticalSection", 
	"SendMessage", 
	"WSAGetLastError",
	"WaitForSingleObject",
	"CloseHandle",
};