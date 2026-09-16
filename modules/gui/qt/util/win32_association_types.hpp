/*****************************************************************************
 * Copyright (C) 1999, 2000 VLC authors and VideoLAN
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef VLC_QT_WIN32_ASSOCIATION_TYPES_HPP
#define VLC_QT_WIN32_ASSOCIATION_TYPES_HPP

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace vlc::win32
{
// Keep .mid out of bulk defaults, matching VLC's Windows installer selection.
inline constexpr std::wstring_view audioExtensions[] = {
    L".3ga", L".669", L".a52", L".aac", L".ac3", L".adt", L".adts", L".aif",
    L".aifc", L".aiff", L".au", L".amr", L".aob", L".ape", L".caf", L".cda",
    L".dts", L".flac", L".it", L".m4a", L".m4p", L".mka", L".mlp",
    L".mod", L".mp1", L".mp2", L".mp3", L".mpc", L".mpga", L".oga", L".oma",
    L".opus", L".qcp", L".ra", L".rmi", L".snd", L".s3m", L".spx", L".tta",
    L".voc", L".vqf", L".w64", L".wav", L".wma", L".wv", L".xa", L".xm",
};

inline constexpr std::wstring_view videoExtensions[] = {
    L".3g2", L".3gp", L".3gp2", L".3gpp", L".amrec", L".amv", L".asf", L".avi",
    L".bik", L".divx", L".drc", L".dv", L".f4v", L".flv", L".gvi", L".gxf",
    L".m1v", L".m2t", L".m2v", L".m2ts", L".m4v", L".mkv", L".mov", L".mp2v",
    L".mp4", L".mp4v", L".mpa", L".mpe", L".mpeg", L".mpeg1", L".mpeg2",
    L".mpeg4", L".mpg", L".mpv2", L".mts", L".mtv", L".mxf", L".nsv", L".nuv",
    L".ogg", L".ogm", L".ogx", L".ogv", L".rec", L".rm", L".rmvb", L".rpl",
    L".thp", L".tod", L".ts", L".tts", L".vob", L".vro", L".webm", L".wmv",
    L".xesc",
};

inline constexpr std::wstring_view playlistExtensions[] = {
    L".asx", L".b4s", L".cue", L".ifo", L".m3u", L".m3u8",
    L".pls", L".ram", L".sdp", L".vlc", L".wvx", L".xspf",
};

inline std::vector<std::wstring> allMediaExtensions()
{
    std::vector<std::wstring> result;
    result.reserve(std::size(audioExtensions) + std::size(videoExtensions) +
                   std::size(playlistExtensions));
    for (const auto extension : audioExtensions)
        result.emplace_back(extension);
    for (const auto extension : videoExtensions)
        result.emplace_back(extension);
    for (const auto extension : playlistExtensions)
        result.emplace_back(extension);
    return result;
}
} // namespace vlc::win32

#endif
