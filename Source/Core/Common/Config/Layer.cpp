// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Config/Layer.h"

namespace Config
{
ConfigLayerLoader::ConfigLayerLoader(LayerType layer) : m_layer(layer)
{
}

ConfigLayerLoader::~ConfigLayerLoader() = default;

LayerType ConfigLayerLoader::GetLayer() const
{
  return m_layer;
}

Layer::Layer(LayerType type) : m_layer(type)
{
}

Layer::Layer(std::unique_ptr<ConfigLayerLoader> loader)
    : m_layer(loader->GetLayer()), m_loader(std::move(loader))
{
  Load();
}

Layer::~Layer()
{
  Save();
}

bool Layer::Exists(const Location& location) const
{
  std::lock_guard lock(m_mutex);
  const auto iter = m_map.find(location);
  return iter != m_map.end() && iter->second.has_value();
}

bool Layer::DeleteKey(const Location& location)
{
  std::lock_guard lock(m_mutex);
  ++m_write_gen;
  m_is_dirty = true;
  bool had_value = false;
  const auto iter = m_map.find(location);
  if (iter != m_map.end() && iter->second.has_value())
  {
    iter->second.reset();
    had_value = true;
  }

  return had_value;
}

void Layer::DeleteAllKeys()
{
  std::lock_guard lock(m_mutex);
  ++m_write_gen;
  m_is_dirty = true;
  for (auto& pair : m_map)
  {
    pair.second.reset();
  }
}

Section Layer::GetSection(System system, const std::string& section)
{
  std::lock_guard lock(m_mutex);
  return Section{m_map.lower_bound(Location{system, section, ""}),
                 m_map.lower_bound(Location{system, section + '\001', ""})};
}

ConstSection Layer::GetSection(System system, const std::string& section) const
{
  std::lock_guard lock(m_mutex);
  return ConstSection{m_map.lower_bound(Location{system, section, ""}),
                      m_map.lower_bound(Location{system, section + '\001', ""})};
}

void Layer::Load()
{
  if (m_loader)
    m_loader->Load(this);
  std::lock_guard lock(m_mutex);
  m_saved_gen = m_write_gen;
  m_is_dirty = false;
}

void Layer::Save()
{
  if (!m_loader)
    return;

  u64 snapshot_gen = 0;
  {
    std::lock_guard lock(m_mutex);
    if (!m_is_dirty)
      return;
    snapshot_gen = m_write_gen;
  }

  // Run the loader outside m_mutex: BaseConfigLayerLoader::Save calls SaveToSYSCONF -> Config::Get,
  // which re-enters Layer::Get on this thread, and GetLayerMap takes a consistent snapshot.
  m_loader->Save(this);

  std::lock_guard lock(m_mutex);
  if (snapshot_gen > m_saved_gen)
    m_saved_gen = snapshot_gen;
  m_is_dirty = (m_write_gen != m_saved_gen);
}

LayerType Layer::GetLayer() const
{
  return m_layer;
}

LayerMap Layer::GetLayerMap() const
{
  std::lock_guard lock(m_mutex);
  return m_map;
}
}  // namespace Config
