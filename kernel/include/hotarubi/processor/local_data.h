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

/* methods dealing directly with the CPU or per-CPU local data */

#ifndef __PROCESSOR_LOCAL_DATA_H
#define __PROCESSOR_LOCAL_DATA_H 1

#include <stdint.h>
#include <hotarubi/gdt.h>

namespace processor
{
	struct local_data
	{
		struct gdt::gdt_pointer    [[aligned(8)]] gdtr;
		struct gdt::gdt_descriptor [[aligned(4)]] gdt[GDT_DESCRIPTOR_COUNT];
	};
};

#endif