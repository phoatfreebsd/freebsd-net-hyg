//===-- SBSection.h ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SBSection_h_
#define LLDB_SBSection_h_

#include "lldb/API/SBDefines.h"
#include "lldb/API/SBData.h"

namespace lldb {

class SBSection
{
public:

    SBSection ();

    SBSection (const lldb::SBSection &rhs);

    ~SBSection ();

    const lldb::SBSection &
    operator = (const lldb::SBSection &rhs);

    bool
    IsValid () const;

    const char *
    GetName ();

    lldb::SBSection
    GetParent();

    lldb::SBSection
    FindSubSection (const char *sect_name);

    size_t
    GetNumSubSections ();

    lldb::SBSection
    GetSubSectionAtIndex (size_t idx);

    lldb::addr_t
    GetFileAddress ();

    lldb::addr_t
    GetLoadAddress (lldb::SBTarget &target);
    
    lldb::addr_t
    GetByteSize ();

    uint64_t
    GetFileOffset ();

    uint64_t
    GetFileByteSize ();
    
    lldb::SBData
    GetSectionData ();

    lldb::SBData
    GetSectionData (uint64_t offset,
                    uint64_t size);
    
    SectionType
    GetSectionType ();

    //------------------------------------------------------------------
    /// Return the size of a target's byte represented by this section
    /// in numbers of host bytes. Note that certain architectures have
    /// varying minimum addressable unit (i.e. byte) size for their 
    /// CODE or DATA buses.
    ///
    /// @return
    ///     The number of host (8-bit) bytes needed to hold a target byte
    //------------------------------------------------------------------
    uint32_t
    GetTargetByteSize ();

    bool
    operator == (const lldb::SBSection &rhs);

    bool
    operator != (const lldb::SBSection &rhs);

    bool
    GetDescription (lldb::SBStream &description);
    

private:

    friend class SBAddress;
    friend class SBModule;
    friend class SBTarget;
    
    SBSection (const lldb::SectionSP &section_sp);

    lldb::SectionSP
    GetSP() const;
    
    void
    SetSP(const lldb::SectionSP &section_sp);
    
    lldb::SectionWP m_opaque_wp;
};


} // namespace lldb

#endif // LLDB_SBSection_h_
