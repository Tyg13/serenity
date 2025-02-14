/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Profile.h"
#include "DisassemblyModel.h"
#include "ProfileModel.h"
#include <AK/HashTable.h>
#include <AK/MappedFile.h>
#include <AK/QuickSort.h>
#include <AK/RefPtr.h>
#include <LibCore/File.h>
#include <LibCoreDump/CoreDumpReader.h>
#include <LibELF/Image.h>
#include <stdio.h>
#include <sys/stat.h>

static void sort_profile_nodes(Vector<NonnullRefPtr<ProfileNode>>& nodes)
{
    quick_sort(nodes.begin(), nodes.end(), [](auto& a, auto& b) {
        return a->event_count() >= b->event_count();
    });

    for (auto& child : nodes)
        child->sort_children();
}

static String object_name(StringView memory_region_name)
{
    if (memory_region_name.contains("Loader.so"))
        return "Loader.so";
    if (!memory_region_name.contains(":"))
        return {};
    return memory_region_name.substring_view(0, memory_region_name.find_first_of(":").value()).to_string();
}

struct CachedLibData {
    OwnPtr<MappedFile> file;
    ELF::Image lib_elf;
};

static String symbolicate(FlatPtr eip, const ELF::Core::MemoryRegionInfo* region, u32& offset)
{

    static HashMap<String, OwnPtr<CachedLibData>> cached_libs;

    StringView region_name { region->region_name };

    auto name = object_name(region_name);

    String path;
    if (name.contains(".so"))
        path = String::format("/usr/lib/%s", name.characters());
    else {
        path = name;
    }

    struct stat st;
    if (stat(path.characters(), &st)) {
        return {};
    }

    if (!cached_libs.contains(path)) {
        auto lib_file = make<MappedFile>(path);
        if (!lib_file->is_valid())
            return {};
        auto image = ELF::Image((const u8*)lib_file->data(), lib_file->size());
        cached_libs.set(path, make<CachedLibData>(move(lib_file), move(image)));
    }

    auto lib_data = cached_libs.get(path).value();

    return String::format("[%s] %s", name.characters(), lib_data->lib_elf.symbolicate(eip - region->region_start, &offset).characters());
}

static String symbolicate_from_coredump(CoreDumpReader& coredump, u32 ptr, [[maybe_unused]] u32& offset)
{
    auto* region = coredump.region_containing((FlatPtr)ptr);
    if (!region) {
        dbgln("did not find region for eip: {:p}", ptr);
        return "??";
    }

    auto name = symbolicate((FlatPtr)ptr, region, offset);
    if (name.is_null()) {
        dbgln("could not symbolicate: {:p}", ptr);
        return "??";
    }
    return name;
}

Profile::Profile(String executable_path, Vector<Event> events)
    : m_executable_path(move(executable_path))
    , m_events(move(events))
{
    m_first_timestamp = m_events.first().timestamp;
    m_last_timestamp = m_events.last().timestamp;

    m_model = ProfileModel::create(*this);

    for (auto& event : m_events) {
        m_deepest_stack_depth = max((u32)event.frames.size(), m_deepest_stack_depth);
    }

    rebuild_tree();
}

Profile::~Profile()
{
}

GUI::Model& Profile::model()
{
    return *m_model;
}

void Profile::rebuild_tree()
{
    u32 filtered_event_count = 0;
    Vector<NonnullRefPtr<ProfileNode>> roots;

    auto find_or_create_root = [&roots](const String& symbol, u32 address, u32 offset, u64 timestamp) -> ProfileNode& {
        for (size_t i = 0; i < roots.size(); ++i) {
            auto& root = roots[i];
            if (root->symbol() == symbol) {
                return root;
            }
        }
        auto new_root = ProfileNode::create(symbol, address, offset, timestamp);
        roots.append(new_root);
        return new_root;
    };

    HashTable<FlatPtr> live_allocations;

    for (auto& event : m_events) {
        if (has_timestamp_filter_range()) {
            auto timestamp = event.timestamp;
            if (timestamp < m_timestamp_filter_range_start || timestamp > m_timestamp_filter_range_end)
                continue;
        }

        if (event.type == "malloc")
            live_allocations.set(event.ptr);
        else if (event.type == "free")
            live_allocations.remove(event.ptr);
    }

    for (size_t event_index = 0; event_index < m_events.size(); ++event_index) {
        auto& event = m_events.at(event_index);
        if (has_timestamp_filter_range()) {
            auto timestamp = event.timestamp;
            if (timestamp < m_timestamp_filter_range_start || timestamp > m_timestamp_filter_range_end)
                continue;
        }

        if (event.type == "malloc" && !live_allocations.contains(event.ptr))
            continue;

        if (event.type == "free")
            continue;

        auto for_each_frame = [&]<typename Callback>(Callback callback) {
            if (!m_inverted) {
                for (size_t i = 0; i < event.frames.size(); ++i) {
                    if (callback(event.frames.at(i), i == event.frames.size() - 1) == IterationDecision::Break)
                        break;
                }
            } else {
                for (ssize_t i = event.frames.size() - 1; i >= 0; --i) {
                    if (callback(event.frames.at(i), static_cast<size_t>(i) == event.frames.size() - 1) == IterationDecision::Break)
                        break;
                }
            }
        };

        if (!m_show_top_functions) {
            ProfileNode* node = nullptr;
            for_each_frame([&](const Frame& frame, bool is_innermost_frame) {
                auto& symbol = frame.symbol;
                auto& address = frame.address;
                auto& offset = frame.offset;

                if (symbol.is_empty())
                    return IterationDecision::Break;

                if (!node)
                    node = &find_or_create_root(symbol, address, offset, event.timestamp);
                else
                    node = &node->find_or_create_child(symbol, address, offset, event.timestamp);

                node->increment_event_count();
                if (is_innermost_frame) {
                    node->add_event_address(address);
                    node->increment_self_count();
                }
                return IterationDecision::Continue;
            });
        } else {
            for (size_t i = 0; i < event.frames.size(); ++i) {
                ProfileNode* node = nullptr;
                ProfileNode* root = nullptr;
                for (size_t j = i; j < event.frames.size(); ++j) {
                    auto& frame = event.frames.at(j);
                    auto& symbol = frame.symbol;
                    auto& address = frame.address;
                    auto& offset = frame.offset;
                    if (symbol.is_empty())
                        break;

                    if (!node) {
                        node = &find_or_create_root(symbol, address, offset, event.timestamp);
                        root = node;
                        root->will_track_seen_events(m_events.size());
                    } else {
                        node = &node->find_or_create_child(symbol, address, offset, event.timestamp);
                    }

                    if (!root->has_seen_event(event_index)) {
                        root->did_see_event(event_index);
                        root->increment_event_count();
                    } else if (node != root) {
                        node->increment_event_count();
                    }

                    if (j == event.frames.size() - 1) {
                        node->add_event_address(address);
                        node->increment_self_count();
                    }
                }
            }
        }

        ++filtered_event_count;
    }

    sort_profile_nodes(roots);

    m_filtered_event_count = filtered_event_count;
    m_roots = move(roots);
    m_model->update();
}

OwnPtr<Profile> Profile::load_from_perfcore_file(const StringView& path)
{
    auto file = Core::File::construct(path);
    if (!file->open(Core::IODevice::ReadOnly)) {
        warnln("Unable to open {}, error: {}", path, file->error_string());
        return nullptr;
    }

    auto json = JsonValue::from_string(file->read_all());
    ASSERT(json.has_value());
    if (!json.value().is_object()) {
        warnln("Invalid perfcore format (not a JSON object)");
        return nullptr;
    }

    auto& object = json.value().as_object();
    auto executable_path = object.get("executable").to_string();

    auto coredump = CoreDumpReader::create(String::format("/tmp/profiler_coredumps/%d", object.get("pid").as_u32()));
    if (!coredump) {
        warnln("Could not open coredump");
        return nullptr;
    }

    MappedFile kernel_elf_file("/boot/Kernel");
    OwnPtr<ELF::Image> kernel_elf;
    if (kernel_elf_file.is_valid())
        kernel_elf = make<ELF::Image>(static_cast<const u8*>(kernel_elf_file.data()), kernel_elf_file.size());

    auto events_value = object.get("events");
    if (!events_value.is_array())
        return nullptr;

    auto& perf_events = events_value.as_array();
    if (perf_events.is_empty())
        return nullptr;

    Vector<Event> events;

    for (auto& perf_event_value : perf_events.values()) {
        auto& perf_event = perf_event_value.as_object();

        Event event;

        event.timestamp = perf_event.get("timestamp").to_number<u64>();
        event.type = perf_event.get("type").to_string();

        if (event.type == "malloc") {
            event.ptr = perf_event.get("ptr").to_number<FlatPtr>();
            event.size = perf_event.get("size").to_number<size_t>();
        } else if (event.type == "free") {
            event.ptr = perf_event.get("ptr").to_number<FlatPtr>();
        }

        auto stack_array = perf_event.get("stack").as_array();
        for (ssize_t i = stack_array.values().size() - 1; i >= 0; --i) {
            auto& frame = stack_array.at(i);
            auto ptr = frame.to_number<u32>();
            u32 offset = 0;
            String symbol;

            if (ptr >= 0xc0000000) {
                if (kernel_elf) {
                    symbol = kernel_elf->symbolicate(ptr, &offset);
                } else {
                    symbol = "??";
                }
            } else {
                symbol = symbolicate_from_coredump(*coredump, ptr, offset);
            }

            event.frames.append({ symbol, ptr, offset });
        }

        if (event.frames.size() < 2)
            continue;

        FlatPtr innermost_frame_address = event.frames.at(1).address;
        event.in_kernel = innermost_frame_address >= 0xc0000000;

        events.append(move(event));
    }

    return NonnullOwnPtr<Profile>(NonnullOwnPtr<Profile>::Adopt, *new Profile(executable_path, move(events)));
}

void ProfileNode::sort_children()
{
    sort_profile_nodes(m_children);
}

void Profile::set_timestamp_filter_range(u64 start, u64 end)
{
    if (m_has_timestamp_filter_range && m_timestamp_filter_range_start == start && m_timestamp_filter_range_end == end)
        return;
    m_has_timestamp_filter_range = true;

    m_timestamp_filter_range_start = min(start, end);
    m_timestamp_filter_range_end = max(start, end);

    rebuild_tree();
}

void Profile::clear_timestamp_filter_range()
{
    if (!m_has_timestamp_filter_range)
        return;
    m_has_timestamp_filter_range = false;
    rebuild_tree();
}

void Profile::set_inverted(bool inverted)
{
    if (m_inverted == inverted)
        return;
    m_inverted = inverted;
    rebuild_tree();
}

void Profile::set_show_top_functions(bool show)
{
    if (m_show_top_functions == show)
        return;
    m_show_top_functions = show;
    rebuild_tree();
}

void Profile::set_show_percentages(bool show_percentages)
{
    if (m_show_percentages == show_percentages)
        return;
    m_show_percentages = show_percentages;
}

void Profile::set_disassembly_index(const GUI::ModelIndex& index)
{
    if (m_disassembly_index == index)
        return;
    m_disassembly_index = index;
    auto* node = static_cast<ProfileNode*>(index.internal_data());
    m_disassembly_model = DisassemblyModel::create(*this, *node);
}

GUI::Model* Profile::disassembly_model()
{
    return m_disassembly_model;
}
