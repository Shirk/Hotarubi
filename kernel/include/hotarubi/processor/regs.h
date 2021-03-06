/*******************************************************************************

    Copyright (C) 2014  René 'Shirk' Köcher
 
    This file is part of Hotarubi.

    Hotarubi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
 
    Hotarubi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*******************************************************************************/

/* processor register access */

#ifndef __PROCESSOR_REGS_H
#define __PROCESSOR_REGS_H 1

#include <hotarubi/types.h>

namespace processor
{
namespace regs
{
	inline uint64_t read_cr0( void )
	{
		uint64_t r;
		__asm__ __volatile__( "movq %%cr0, %0" : "=r"( r ) );
		return r;
	};

	inline uint64_t read_cr1( void )
	{
		uint64_t r;
		__asm__ __volatile__( "movq %%cr1, %0" : "=r"( r ) );
		return r;
	};

	inline uint64_t read_cr2( void )
	{
		uint64_t r;
		__asm__ __volatile__( "movq %%cr2, %0" : "=r"( r ) );
		return r;
	};

	inline uint64_t read_cr3( void )
	{
		uint64_t r;
		__asm__ __volatile__( "movq %%cr3, %0" : "=r"( r ) );
		return r;
	};

	inline uint64_t read_cr4( void )
	{
		uint64_t r;
		__asm__ __volatile__( "movq %%cr4, %0" : "=r"( r ) );
		return r;
	};

	inline void write_cr0( uint64_t val )
	{
		__asm__ __volatile__( "movq %0, %%cr0" :: "r"( val ) );
	};

	inline void write_cr1( uint64_t val )
	{
		__asm__ __volatile__( "movq %0, %%cr1" :: "r"( val ) );
	};

	inline void write_cr2( uint64_t val )
	{
		__asm__ __volatile__( "movq %0, %%cr2" :: "r"( val ) );
	};

	inline void write_cr3( uint64_t val )
	{
		__asm__ __volatile__( "movq %0, %%cr3" :: "r"( val ) );
	};

	inline void write_cr4( uint64_t val )
	{
		__asm__ __volatile__( "movq %0, %%cr4" :: "r"( val ) );
	};

	inline uint64_t read_msr( uint32_t reg )
	{
		uint32_t lo, hi;
		__asm__ __volatile__( "rdmsr" : "=d"( hi ), "=a"( lo ) : "c"( reg ) );

		return ( ( uint64_t ) hi << 32 ) | lo;
	};

	inline void write_msr( uint32_t reg, uint64_t val )
	{
		uint32_t lo = val & 0xffffffff, 
		         hi = val >> 32;

		__asm__ __volatile__( "wrmsr" :: "c"( reg ), "d"( hi ), "a"( lo ) );
	};
};
};

#endif
