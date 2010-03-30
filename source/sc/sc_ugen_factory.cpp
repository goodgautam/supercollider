//  prototype of a supercollider-synthdef-based synth prototype
//  Copyright (C) 2009 Tim Blechmann
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//  Boston, MA 02111-1307, USA.

/* \todo for now we use dlopen, later we want to become cross-platform
 */

#ifdef DLOPEN
#include <dlfcn.h>
#endif

#include <boost/filesystem.hpp>

#include "sc_ugen_factory.hpp"

#include "supercollider/common/Headers/plugin_interface/SC_InterfaceTable.h"
#include "supercollider/common/Headers/plugin_interface/SC_World.h"
#include "supercollider/common/Headers/plugin_interface/SC_Wire.h"

namespace nova
{


sc_ugen_factory sc_factory;

sc_ugen_def::sc_ugen_def (const char *inUnitClassName, size_t inAllocSize,
                          UnitCtorFunc inCtor, UnitDtorFunc inDtor, uint32 inFlags):
    name_(inUnitClassName), alloc_size(inAllocSize), ctor(inCtor), dtor(inDtor), flags(inFlags)
{}

Unit * sc_ugen_def::construct(sc_synthdef::unit_spec_t const & unit_spec, sc_synth * s, World * world, char *& chunk)
{
    const size_t output_count = unit_spec.output_specs.size();

    /* size for wires and buffers */
    Unit * unit = (Unit*)chunk;     chunk += alloc_size;
    unit->mInput  = (Wire**)chunk;  chunk += unit_spec.input_specs.size() * sizeof(Wire*);
    unit->mOutput = (Wire**)chunk;  chunk += unit_spec.output_specs.size() * sizeof(Wire*);
    unit->mInBuf  = (float**)chunk; chunk += unit_spec.input_specs.size() * sizeof(float*);
    unit->mOutBuf = (float**)chunk; chunk += unit_spec.output_specs.size() * sizeof(float*);

    unit->mNumInputs  = unit_spec.input_specs.size();
    unit->mNumOutputs = unit_spec.output_specs.size();

    /* initialize members */
    unit->mCalcRate = unit_spec.rate;
    unit->mSpecialIndex = unit_spec.special_index;
    unit->mDone = false;
    unit->mUnitDef = reinterpret_cast<struct UnitDef*>(this); /* we abuse this field to store our reference */
    unit->mWorld = world;

    /* initialize members from synth */
    unit->mParent = static_cast<Graph*>(s);
    if (unit_spec.rate == 2)
        unit->mRate = &s->full_rate;
    else
        unit->mRate = &s->control_rate;

    unit->mBufLength = unit->mRate->mBufLength;

    float * buffer_base = (float*) ((size_t(s->unit_buffers) + 63) & ~63); /* next multiple of 64 */

    /* allocate buffers */
    for (size_t i = 0; i != output_count; ++i) {
        Wire * w = (Wire*)chunk; chunk += sizeof(Wire);

        w->mFromUnit = unit;
        w->mCalcRate = unit->mCalcRate;

        w->mBuffer = 0;
        w->mScalarValue = 0;

        if (unit->mCalcRate == 2) {
            /* allocate a new buffer */
            assert(unit_spec.buffer_mapping[i] >= 0);
            std::size_t buffer_id = unit_spec.buffer_mapping[i];
            unit->mOutBuf[i] = buffer_base + 64 * buffer_id;
            w->mBuffer = unit->mOutBuf[i];
        }
        else
            unit->mOutBuf[i] = &w->mScalarValue;

        unit->mOutput[i] = w;
    }

    /* prepare inputs */
    for (size_t i = 0; i != unit_spec.input_specs.size(); ++i)
    {
        int source = unit_spec.input_specs[i].source;
        int index = unit_spec.input_specs[i].index;

        if (source == -1)
            unit->mInput[i] = &unit->mParent->mWire[index];
        else
        {
            Unit * prev = s->units[source];
            unit->mInput[i] = prev->mOutput[index];
        }

        if (unit->mInput[i]->mCalcRate == 2)
            unit->mInBuf[i] = unit->mInput[i]->mBuffer;
        else
            unit->mInBuf[i] = &unit->mInput[i]->mScalarValue;
    }

    return unit;
}

void sc_ugen_def::destruct(Unit * unit)
{
    if (dtor)
        (*dtor)(unit);
}

void sc_ugen_factory::load_plugin_folder (boost::filesystem::path const & path)
{
    using namespace boost::filesystem;

    directory_iterator end;

    if (!is_directory(path))
        return;

    for (directory_iterator it(path); it != end; ++it) {
        if (is_regular_file(it->status()))
            load_plugin(it->path());
    }
}


#ifdef DLOPEN
void sc_ugen_factory::load_plugin ( boost::filesystem::path const & path )
{
    void * handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
        return;

    void * load_symbol = dlsym(handle, "load");
    if (!load_symbol) {
        dlclose(handle);
        return;
    }

    open_handles.push_back(handle);
    LoadPlugInFunc load_func = reinterpret_cast<LoadPlugInFunc>(load_symbol);
    (*load_func)(&sc_interface);

    /* don't close the handle */
    return;
}

void sc_ugen_factory::close_handles(void)
{
#if 0
    /* closing the handles has some unwanted side effects, so we leave them open */
    foreach(void * handle, open_handles)
        dlclose(handle);
#endif
}

#else
void sc_ugen_factory::load_plugin ( boost::filesystem::path const & path )
{
}

void sc_ugen_factory::close_handles(void)
{}
#endif

void sc_ugen_factory::register_ugen(const char *inUnitClassName, size_t inAllocSize,
                                    UnitCtorFunc inCtor, UnitDtorFunc inDtor, uint32 inFlags)
{
    sc_ugen_def * def = new sc_ugen_def(inUnitClassName, inAllocSize, inCtor, inDtor, inFlags);
    ugen_set.insert(*def);
}

void sc_ugen_factory::register_bufgen(const char * name, BufGenFunc func)
{
    sc_bufgen_def * def = new sc_bufgen_def(name, func);
    bufgen_map.insert(*def);
}

BufGenFunc sc_ugen_factory::find_bufgen(const char * name)
{
    bufgen_set_t::iterator it = bufgen_map.find(name,
                                                compare_def<sc_bufgen_def>());
    if (it == bufgen_map.end()) {
        std::cerr << "unable to find buffer generator: " << name << std::endl;
        throw std::runtime_error("unable to create ugen");
    }
    return it->func;
}

sc_ugen_def * sc_ugen_factory::find_ugen(std::string const & name)
{
    ugen_set_type::iterator it = ugen_set.find(name,
                                               hash_def<sc_ugen_def>(), equal_def<sc_ugen_def>());
    if (it == ugen_set.end()) {
        std::cerr << "ugen not registered: " << name << std::endl;
        return 0;
    }
    return &*it;
}


void sc_ugen_factory::free_ugen(struct Unit * unit)
{
    sc_ugen_def * def = reinterpret_cast<sc_ugen_def*>(unit->mUnitDef);
    def->destruct(unit);
    --ugen_count_;
}

} /* namespace nova */
