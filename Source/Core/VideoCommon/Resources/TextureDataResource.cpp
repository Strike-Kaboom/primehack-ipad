// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Resources/TextureDataResource.h"

#include "VideoCommon/Assets/CustomAssetCache.h"

namespace VideoCommon
{
TextureDataResource::TextureDataResource(Resource::ResourceContext resource_context)
    : Resource(std::move(resource_context))
{
  m_texture_asset = m_resource_context.asset_cache->CreateAsset<TextureAsset>(
      m_resource_context.primary_asset_id, m_resource_context.asset_library, this);
}

std::shared_ptr<CustomTextureData> TextureDataResource::GetData() const
{
  return m_current_texture_data;
}

CustomAsset::TimeType TextureDataResource::GetLoadTime() const
{
  return m_current_time;
}

Resource::TaskComplete TextureDataResource::CollectPrimaryData()
{
  const auto last_load_time = m_texture_asset->GetLastLoadedTime();
  const auto asset = m_texture_asset->GetData();
  if (!asset)
    return Resource::TaskComplete::No;

  m_current_texture_data = asset;
  m_current_time = last_load_time;
  return Resource::TaskComplete::Yes;
}

void TextureDataResource::OnUnloadRequested()
{
  // PrimeHack for iPad: the asset cache dropped its copy to get back under its memory limit. This
  // resource shares that buffer, so without dropping it here too the bytes were never freed: the
  // cache's accounting fell while real memory only grew with every texture the session touched.
  // Start over, so the texture is requested again the next time the game binds it (the original
  // texture shows until it has loaded, as on first use).
  m_current_texture_data.reset();
  m_current_time = {};
  RestartProcessing();
}

void TextureDataResource::MarkAsActive()
{
  m_resource_context.asset_cache->MarkAssetActive(m_texture_asset);
}

void TextureDataResource::MarkAsPending()
{
  m_resource_context.asset_cache->MarkAssetPending(m_texture_asset);
}
}  // namespace VideoCommon
