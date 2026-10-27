//! Sega Saturn disc-image reader.
//!
//! M0 supports plain ISO images and the first data track of BIN/CUE images.
//! The filesystem layer intentionally exposes bytes and directory metadata;
//! SH-2 decoding remains a separate frontend.

use std::fmt;
use std::fs;
use std::path::Path;

const LOGICAL_SECTOR_SIZE: usize = 2048;
const ISO_PVD_LBA: u32 = 16;
const SATURN_HARDWARE_ID: &[u8; 16] = b"SEGA SEGASATURN ";
pub const IP_LOAD_ADDRESS: u32 = 0x0600_2000;
const SYSTEM_ID_SIZE: u32 = 0x100;
const SECURITY_CODE_SIZE: u32 = 0xd00;
const AREA_CODE_SIZE: u32 = 0x20;

#[derive(Debug)]
pub enum DiscError {
    Io(std::io::Error),
    InvalidImage(String),
    InvalidCue(String),
    FileNotFound(String),
    NotIso9660,
}

impl fmt::Display for DiscError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(f, "I/O error: {error}"),
            Self::InvalidImage(message) => write!(f, "invalid disc image: {message}"),
            Self::InvalidCue(message) => write!(f, "invalid CUE sheet: {message}"),
            Self::FileNotFound(path) => write!(f, "file not found in image: {path}"),
            Self::NotIso9660 => write!(f, "image does not contain an ISO 9660 filesystem"),
        }
    }
}

impl std::error::Error for DiscError {}

impl From<std::io::Error> for DiscError {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SectorLayout {
    PlainIso,
    RawMode1,
    RawMode2,
    RawMode1Subchannel,
}

impl SectorLayout {
    fn sector_size(self) -> usize {
        match self {
            Self::PlainIso => 2048,
            Self::RawMode1 | Self::RawMode2 => 2352,
            Self::RawMode1Subchannel => 2448,
        }
    }

    fn user_data_offset(self) -> usize {
        match self {
            Self::PlainIso => 0,
            Self::RawMode1 | Self::RawMode1Subchannel => 16,
            Self::RawMode2 => 24,
        }
    }
}

#[derive(Clone, Debug)]
struct TrackImage {
    bytes: Vec<u8>,
    file_offset: usize,
    layout: SectorLayout,
}

/// A mounted Saturn data track and its ISO 9660 filesystem.
#[derive(Clone, Debug)]
pub struct DiscImage {
    track: TrackImage,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IsoEntry {
    pub path: String,
    pub extent_lba: u32,
    pub size: u32,
    pub is_directory: bool,
}

/// Parsed fields from the Saturn System ID sector (the first 256 bytes of IP.BIN).
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SaturnBootInfo {
    pub hardware_id: String,
    pub maker_id: String,
    pub product_number: String,
    pub version: String,
    pub release_date: String,
    pub device_info: String,
    pub compatible_areas: String,
    pub compatible_peripherals: String,
    pub game_title: String,
    pub ip_size: u32,
    pub master_stack_address: u32,
    pub slave_stack_address: u32,
    /// Destination of the first ISO 9660 root file. This is a load address,
    /// not an entry point: the Application Initial Program performs the jump.
    pub first_read_address: u32,
    /// Defined by the format, but ignored by the Saturn boot ROM for CD media.
    pub first_read_size: u32,
}

impl SaturnBootInfo {
    /// Number of concatenated Saturn area symbols and area-code objects.
    pub fn area_code_count(&self) -> u32 {
        self.compatible_areas
            .bytes()
            .filter(|value| !value.is_ascii_whitespace())
            .count() as u32
    }

    /// Byte offset of the Application Initial Program inside IP.BIN.
    pub fn aip_offset(&self) -> u32 {
        SYSTEM_ID_SIZE + SECURITY_CODE_SIZE + self.area_code_count() * AREA_CODE_SIZE
    }

    /// Runtime address at which the boot ROM starts the AIP.
    pub fn aip_address(&self) -> u32 {
        IP_LOAD_ADDRESS + self.aip_offset()
    }
}

impl DiscImage {
    /// Opens `.iso`, raw `.bin`, or a `.cue` sheet's first data track.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, DiscError> {
        let path = path.as_ref();
        let extension = path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or("");
        if extension.eq_ignore_ascii_case("cue") {
            Self::open_cue(path)
        } else {
            Self::from_bytes(fs::read(path)?)
        }
    }

    /// Opens an image already loaded in memory. Useful for callers and tests.
    pub fn from_bytes(bytes: Vec<u8>) -> Result<Self, DiscError> {
        for layout in [
            SectorLayout::PlainIso,
            SectorLayout::RawMode1,
            SectorLayout::RawMode2,
            SectorLayout::RawMode1Subchannel,
        ] {
            if valid_pvd(&bytes, 0, layout) {
                return Self::from_track(TrackImage {
                    bytes,
                    file_offset: 0,
                    layout,
                });
            }
        }

        // MDF files can contain a small proprietary/header prefix before the
        // first raw sector. Locate the PVD signature and derive that prefix
        // instead of assuming the track starts at byte zero.
        for (signature, window) in bytes.windows(6).enumerate() {
            if window != [1, b'C', b'D', b'0', b'0', b'1'] {
                continue;
            }
            let pvd_start = signature;
            for layout in [
                SectorLayout::RawMode1,
                SectorLayout::RawMode2,
                SectorLayout::RawMode1Subchannel,
            ] {
                let header =
                    layout.user_data_offset() + ISO_PVD_LBA as usize * layout.sector_size();
                if pvd_start < header || !valid_pvd(&bytes, pvd_start - header, layout) {
                    continue;
                }
                return Self::from_track(TrackImage {
                    bytes,
                    file_offset: pvd_start - header,
                    layout,
                });
            }
        }
        Err(DiscError::NotIso9660)
    }

    fn from_track(track: TrackImage) -> Result<Self, DiscError> {
        let image = Self { track };
        image.root_record()?;
        Ok(image)
    }

    fn open_cue(path: &Path) -> Result<Self, DiscError> {
        let text = fs::read_to_string(path)?;
        let parent = path.parent().unwrap_or_else(|| Path::new("."));
        let mut file_name: Option<String> = None;
        let mut track_mode: Option<String> = None;
        let mut index01_sectors = 0usize;
        for raw_line in text.lines() {
            let line = raw_line.trim();
            let upper = line.to_ascii_uppercase();
            if upper.starts_with("FILE ") {
                let value = line[5..].trim();
                file_name = Some(if let Some(stripped) = value.strip_prefix('"') {
                    stripped.split('"').next().unwrap_or("").to_string()
                } else {
                    value.split_whitespace().next().unwrap_or("").to_string()
                });
            } else if upper.starts_with("TRACK ") && upper.contains("MODE") && track_mode.is_none()
            {
                track_mode = upper.split_whitespace().nth(2).map(str::to_string);
            } else if upper.starts_with("INDEX 01 ") && track_mode.is_some() {
                index01_sectors = parse_msf(line[9..].trim())?;
            }
        }
        let file_name = file_name.ok_or_else(|| DiscError::InvalidCue("missing FILE".into()))?;
        let mode = track_mode.ok_or_else(|| DiscError::InvalidCue("missing data TRACK".into()))?;
        let layout = match mode.as_str() {
            "MODE1/2048" => SectorLayout::PlainIso,
            "MODE1/2352" => SectorLayout::RawMode1,
            "MODE2/2352" => SectorLayout::RawMode2,
            other => {
                return Err(DiscError::InvalidCue(format!(
                    "unsupported data mode {other}"
                )))
            }
        };
        let bytes = fs::read(parent.join(file_name))?;
        Self::from_track(TrackImage {
            bytes,
            file_offset: index01_sectors * layout.sector_size(),
            layout,
        })
    }

    fn logical_sector(&self, lba: u32) -> Result<&[u8], DiscError> {
        let size = self.track.layout.sector_size();
        let start = self
            .track
            .file_offset
            .checked_add(lba as usize * size)
            .and_then(|value| value.checked_add(self.track.layout.user_data_offset()))
            .ok_or_else(|| DiscError::InvalidImage("sector address overflow".into()))?;
        let end = start
            .checked_add(LOGICAL_SECTOR_SIZE)
            .ok_or_else(|| DiscError::InvalidImage("sector address overflow".into()))?;
        self.track
            .bytes
            .get(start..end)
            .ok_or_else(|| DiscError::InvalidImage(format!("sector {lba} is outside image")))
    }

    fn read_range(&self, lba: u32, offset: usize, length: usize) -> Result<Vec<u8>, DiscError> {
        let mut result = Vec::with_capacity(length);
        let mut position = offset;
        while result.len() < length {
            let sector = self.logical_sector(lba + (position / LOGICAL_SECTOR_SIZE) as u32)?;
            let within = position % LOGICAL_SECTOR_SIZE;
            let amount = (LOGICAL_SECTOR_SIZE - within).min(length - result.len());
            result.extend_from_slice(&sector[within..within + amount]);
            position += amount;
        }
        Ok(result)
    }

    /// Parses the Sega Saturn System ID in logical sector zero.
    pub fn boot_info(&self) -> Result<SaturnBootInfo, DiscError> {
        let sector = self.logical_sector(0)?;
        if &sector[..SATURN_HARDWARE_ID.len()] != SATURN_HARDWARE_ID {
            return Err(DiscError::InvalidImage(
                "missing SEGA SEGASATURN System ID".into(),
            ));
        }
        let ip_size = be_u32(&sector[0xe0..0xe4]);
        if !(0x1000..=0x8000).contains(&ip_size) {
            return Err(DiscError::InvalidImage(format!(
                "IP size 0x{ip_size:08X} is outside the Saturn range"
            )));
        }
        Ok(SaturnBootInfo {
            hardware_id: ascii_field(&sector[0x00..0x10]),
            maker_id: ascii_field(&sector[0x10..0x20]),
            product_number: ascii_field(&sector[0x20..0x2a]),
            version: ascii_field(&sector[0x2a..0x30]),
            release_date: ascii_field(&sector[0x30..0x38]),
            device_info: ascii_field(&sector[0x38..0x40]),
            compatible_areas: ascii_field(&sector[0x40..0x4a]),
            compatible_peripherals: ascii_field(&sector[0x50..0x60]),
            game_title: ascii_field(&sector[0x60..0xd0]),
            ip_size,
            master_stack_address: be_u32(&sector[0xe8..0xec]),
            slave_stack_address: be_u32(&sector[0xec..0xf0]),
            first_read_address: be_u32(&sector[0xf0..0xf4]),
            first_read_size: be_u32(&sector[0xf4..0xf8]),
        })
    }

    /// Returns the complete Initial Program loaded at 0x06002000 by the boot ROM.
    pub fn initial_program(&self) -> Result<Vec<u8>, DiscError> {
        let info = self.boot_info()?;
        self.read_range(0, 0, info.ip_size as usize)
    }

    /// Returns only the executable Application Initial Program portion of IP.BIN.
    pub fn application_initial_program(&self) -> Result<Vec<u8>, DiscError> {
        let info = self.boot_info()?;
        let offset = info.aip_offset();
        if offset >= info.ip_size {
            return Err(DiscError::InvalidImage(format!(
                "AIP offset 0x{offset:X} is outside IP size 0x{:X}",
                info.ip_size
            )));
        }
        self.read_range(0, offset as usize, (info.ip_size - offset) as usize)
    }

    fn root_record(&self) -> Result<(u32, u32), DiscError> {
        let pvd = self.logical_sector(ISO_PVD_LBA)?;
        let length = pvd[156] as usize;
        if length < 34 || 156 + length > pvd.len() {
            return Err(DiscError::InvalidImage(
                "invalid root directory record".into(),
            ));
        }
        Ok((le_u32(&pvd[158..162]), le_u32(&pvd[166..170])))
    }

    /// Returns all files and directories, using normalized `/`-separated paths.
    pub fn entries(&self) -> Result<Vec<IsoEntry>, DiscError> {
        let (lba, size) = self.root_record()?;
        let mut entries = Vec::new();
        self.walk_directory(lba, size, "", 0, &mut entries)?;
        Ok(entries)
    }

    /// Returns the first regular file recorded in the ISO 9660 root directory.
    /// Saturn calls this file identifier [2] and transfers it to FIRST READ ADDRESS.
    pub fn first_read_file(&self) -> Result<IsoEntry, DiscError> {
        self.entries()?
            .into_iter()
            .find(|entry| !entry.is_directory && !entry.path.contains('/'))
            .ok_or_else(|| DiscError::FileNotFound("first root-directory file".into()))
    }

    fn walk_directory(
        &self,
        lba: u32,
        size: u32,
        prefix: &str,
        depth: usize,
        entries: &mut Vec<IsoEntry>,
    ) -> Result<(), DiscError> {
        if depth > 32 {
            return Err(DiscError::InvalidImage("directory nesting too deep".into()));
        }
        let data = self.read_range(lba, 0, size as usize)?;
        let mut offset = 0usize;
        while offset < data.len() {
            let length = data[offset] as usize;
            if length == 0 {
                offset = ((offset / LOGICAL_SECTOR_SIZE) + 1) * LOGICAL_SECTOR_SIZE;
                continue;
            }
            if length < 34 || offset + length > data.len() {
                return Err(DiscError::InvalidImage("invalid directory record".into()));
            }
            let record = &data[offset..offset + length];
            let flags = record[25];
            let name_length = record[32] as usize;
            if 33 + name_length > record.len() {
                return Err(DiscError::InvalidImage("invalid directory name".into()));
            }
            let raw_name = &record[33..33 + name_length];
            if raw_name != [0] && raw_name != [1] {
                let name = normalize_name(raw_name);
                let path = if prefix.is_empty() {
                    name.clone()
                } else {
                    format!("{prefix}/{name}")
                };
                let entry = IsoEntry {
                    path: path.clone(),
                    extent_lba: le_u32(&record[2..6]),
                    size: le_u32(&record[10..14]),
                    is_directory: flags & 0x02 != 0,
                };
                entries.push(entry.clone());
                if entry.is_directory {
                    self.walk_directory(entry.extent_lba, entry.size, &path, depth + 1, entries)?;
                }
            }
            offset += length;
        }
        Ok(())
    }

    pub fn find_file(&self, path: &str) -> Result<IsoEntry, DiscError> {
        let wanted = path.trim_matches('/').to_ascii_uppercase();
        self.entries()?
            .into_iter()
            .find(|entry| entry.path.to_ascii_uppercase() == wanted && !entry.is_directory)
            .ok_or_else(|| DiscError::FileNotFound(path.into()))
    }

    pub fn extract_file(&self, path: &str) -> Result<Vec<u8>, DiscError> {
        let entry = self.find_file(path)?;
        self.read_range(entry.extent_lba, 0, entry.size as usize)
    }

    /// Picks a likely Saturn program file, excluding the boot IP.BIN when possible.
    pub fn find_executable(&self) -> Result<IsoEntry, DiscError> {
        let mut files: Vec<_> = self
            .entries()?
            .into_iter()
            .filter(|entry| !entry.is_directory)
            .collect();
        files.sort_by_key(|entry| {
            let name = entry.path.to_ascii_uppercase();
            let exact = ["0.BIN", "PROGRAM.BIN", "MAIN.BIN", "GAME.BIN"]
                .iter()
                .any(|candidate| name == *candidate);
            let extension = name.ends_with(".BIN") || name.ends_with(".EXE");
            let score = if exact {
                3
            } else if extension && !name.ends_with("IP.BIN") {
                2
            } else {
                0
            };
            (score, entry.size)
        });
        files
            .into_iter()
            .rev()
            .find(|entry| {
                let name = entry.path.to_ascii_uppercase();
                (name.ends_with(".BIN") || name.ends_with(".EXE")) && !name.ends_with("IP.BIN")
            })
            .ok_or_else(|| {
                DiscError::FileNotFound("main executable (no .BIN/.EXE candidate)".into())
            })
    }
}

fn le_u32(value: &[u8]) -> u32 {
    u32::from_le_bytes([value[0], value[1], value[2], value[3]])
}

fn be_u32(value: &[u8]) -> u32 {
    u32::from_be_bytes([value[0], value[1], value[2], value[3]])
}

fn ascii_field(value: &[u8]) -> String {
    String::from_utf8_lossy(value)
        .trim_matches(['\0', ' '])
        .to_string()
}

fn valid_pvd(bytes: &[u8], file_offset: usize, layout: SectorLayout) -> bool {
    let pvd = match file_offset
        .checked_add(layout.user_data_offset())
        .and_then(|value| value.checked_add(ISO_PVD_LBA as usize * layout.sector_size()))
    {
        Some(value) => value,
        None => return false,
    };
    if bytes.len() < pvd + LOGICAL_SECTOR_SIZE
        || bytes[pvd] != 1
        || bytes[pvd + 1..pvd + 6] != *b"CD001"
    {
        return false;
    }
    let root_length = bytes[pvd + 156] as usize;
    if root_length < 34 || pvd + 156 + root_length > pvd + LOGICAL_SECTOR_SIZE {
        return false;
    }
    let root_lba = le_u32(&bytes[pvd + 158..pvd + 162]);
    let root_offset = match file_offset
        .checked_add(layout.user_data_offset())
        .and_then(|value| value.checked_add(root_lba as usize * layout.sector_size()))
    {
        Some(value) => value,
        None => return false,
    };
    bytes
        .get(root_offset..root_offset + 34)
        .is_some_and(|record| {
            let length = record[0] as usize;
            let name_length = record[32] as usize;
            length >= 34
                && length <= LOGICAL_SECTOR_SIZE
                && name_length > 0
                && 33 + name_length <= length
        })
}

fn normalize_name(raw: &[u8]) -> String {
    let text = String::from_utf8_lossy(raw)
        .trim_end_matches(' ')
        .to_string();
    text.strip_suffix(";1").unwrap_or(&text).to_string()
}

fn parse_msf(value: &str) -> Result<usize, DiscError> {
    let parts: Vec<_> = value.split(':').collect();
    if parts.len() != 3 {
        return Err(DiscError::InvalidCue("invalid INDEX time".into()));
    }
    let minutes: usize = parts[0]
        .parse()
        .map_err(|_| DiscError::InvalidCue("invalid INDEX minutes".into()))?;
    let seconds: usize = parts[1]
        .parse()
        .map_err(|_| DiscError::InvalidCue("invalid INDEX seconds".into()))?;
    let frames: usize = parts[2]
        .parse()
        .map_err(|_| DiscError::InvalidCue("invalid INDEX frames".into()))?;
    if seconds >= 60 || frames >= 75 {
        return Err(DiscError::InvalidCue("invalid INDEX range".into()));
    }
    Ok((minutes * 60 + seconds) * 75 + frames)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record(name: &[u8], lba: u32, size: u32, directory: bool) -> Vec<u8> {
        let length = 33 + name.len();
        let mut value = vec![0u8; length + (length & 1)];
        value[0] = length as u8;
        value[2..6].copy_from_slice(&lba.to_le_bytes());
        value[10..14].copy_from_slice(&size.to_le_bytes());
        value[25] = if directory { 2 } else { 0 };
        value[32] = name.len() as u8;
        value[33..33 + name.len()].copy_from_slice(name);
        value
    }

    #[test]
    fn parses_iso_directory_and_extracts_program() {
        let mut image = vec![0u8; 20 * LOGICAL_SECTOR_SIZE];
        image[..16].copy_from_slice(SATURN_HARDWARE_ID);
        image[0x10..0x20].copy_from_slice(b"SEGA ENTERPRISES");
        image[0x20..0x2a].copy_from_slice(b"T-0000G   ");
        image[0x2a..0x30].copy_from_slice(b"V1.000");
        image[0x30..0x38].copy_from_slice(b"19960101");
        image[0x40..0x4a].copy_from_slice(b"J         ");
        image[0x60..0x6a].copy_from_slice(b"TEST GAME ");
        image[0xe0..0xe4].copy_from_slice(&0x1000u32.to_be_bytes());
        image[0xe8..0xec].copy_from_slice(&0x0600_1000u32.to_be_bytes());
        image[0xec..0xf0].copy_from_slice(&0x0600_2000u32.to_be_bytes());
        image[0xf0..0xf4].copy_from_slice(&0x0600_9000u32.to_be_bytes());
        image[16 * LOGICAL_SECTOR_SIZE] = 1;
        image[16 * LOGICAL_SECTOR_SIZE + 1..16 * LOGICAL_SECTOR_SIZE + 6].copy_from_slice(b"CD001");
        let root_lba = 17u32;
        let root_size = 2048u32;
        let pvd = 16 * LOGICAL_SECTOR_SIZE;
        let root = record(&[0], root_lba, root_size, true);
        image[pvd + 156..pvd + 156 + root.len()].copy_from_slice(&root);
        let file = record(b"GAME.BIN;1", 18, 4, false);
        image[root_lba as usize * LOGICAL_SECTOR_SIZE
            ..root_lba as usize * LOGICAL_SECTOR_SIZE + file.len()]
            .copy_from_slice(&file);
        image[18 * LOGICAL_SECTOR_SIZE..18 * LOGICAL_SECTOR_SIZE + 4].copy_from_slice(b"SH2!");
        let disc = DiscImage::from_bytes(image).unwrap();
        let boot = disc.boot_info().unwrap();
        assert_eq!(boot.game_title, "TEST GAME");
        assert_eq!(boot.ip_size, 0x1000);
        assert_eq!(boot.first_read_address, 0x0600_9000);
        assert_eq!(boot.area_code_count(), 1);
        assert_eq!(boot.aip_offset(), 0xe20);
        assert_eq!(boot.aip_address(), 0x0600_2e20);
        assert_eq!(disc.initial_program().unwrap().len(), 0x1000);
        assert_eq!(disc.application_initial_program().unwrap().len(), 0x1e0);
        assert_eq!(disc.first_read_file().unwrap().path, "GAME.BIN");
        assert_eq!(disc.find_executable().unwrap().path, "GAME.BIN");
        assert_eq!(disc.extract_file("/game.bin").unwrap(), b"SH2!");
    }

    #[test]
    fn detects_raw_mdf_with_track_prefix() {
        let mut iso = vec![0u8; 20 * LOGICAL_SECTOR_SIZE];
        iso[16 * LOGICAL_SECTOR_SIZE] = 1;
        iso[16 * LOGICAL_SECTOR_SIZE + 1..16 * LOGICAL_SECTOR_SIZE + 6].copy_from_slice(b"CD001");
        let root = record(&[0], 17, 2048, true);
        iso[16 * LOGICAL_SECTOR_SIZE + 156..16 * LOGICAL_SECTOR_SIZE + 156 + root.len()]
            .copy_from_slice(&root);
        iso[17 * LOGICAL_SECTOR_SIZE..17 * LOGICAL_SECTOR_SIZE + root.len()].copy_from_slice(&root);
        let file = record(b"0.BIN;1", 18, 4, false);
        iso[17 * LOGICAL_SECTOR_SIZE..17 * LOGICAL_SECTOR_SIZE + file.len()].copy_from_slice(&file);
        iso[18 * LOGICAL_SECTOR_SIZE..18 * LOGICAL_SECTOR_SIZE + 4].copy_from_slice(b"GAME");

        let prefix = 1536usize;
        let mut raw = vec![0u8; prefix + 20 * 2352];
        for lba in 0..20usize {
            let source = &iso[lba * LOGICAL_SECTOR_SIZE..(lba + 1) * LOGICAL_SECTOR_SIZE];
            let destination = prefix + lba * 2352 + 16;
            raw[destination..destination + LOGICAL_SECTOR_SIZE].copy_from_slice(source);
        }
        let disc = DiscImage::from_bytes(raw).unwrap();
        assert_eq!(disc.extract_file("0.BIN").unwrap(), b"GAME");
    }

    #[test]
    fn detects_raw_mdf_with_subchannel_bytes() {
        let mut iso = vec![0u8; 18 * LOGICAL_SECTOR_SIZE];
        iso[16 * LOGICAL_SECTOR_SIZE] = 1;
        iso[16 * LOGICAL_SECTOR_SIZE + 1..16 * LOGICAL_SECTOR_SIZE + 6].copy_from_slice(b"CD001");
        let root = record(&[0], 17, 2048, true);
        iso[16 * LOGICAL_SECTOR_SIZE + 156..16 * LOGICAL_SECTOR_SIZE + 156 + root.len()]
            .copy_from_slice(&root);
        iso[17 * LOGICAL_SECTOR_SIZE..17 * LOGICAL_SECTOR_SIZE + root.len()].copy_from_slice(&root);
        let mut raw = vec![0u8; 18 * 2448];
        for lba in 0..18usize {
            let source = &iso[lba * LOGICAL_SECTOR_SIZE..(lba + 1) * LOGICAL_SECTOR_SIZE];
            let destination = lba * 2448 + 16;
            raw[destination..destination + LOGICAL_SECTOR_SIZE].copy_from_slice(source);
        }
        assert!(DiscImage::from_bytes(raw).is_ok());
    }
}
