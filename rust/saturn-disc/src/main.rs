use saturn_disc::{DiscError, DiscImage};
use std::env;
use std::fs;

fn usage() {
    eprintln!(
        "Usage:\n  saturn-disc inspect <image>\n  saturn-disc list <image> [--extents]\n  saturn-disc extract <image> [path] --output <file>\n  saturn-disc extract-ip <image> --output <ip.bin>\n  saturn-disc extract-aip <image> --output <aip.bin>"
    );
}

fn output_argument(args: &[String]) -> Result<&str, DiscError> {
    args.windows(2)
        .find(|pair| pair[0] == "--output")
        .map(|pair| pair[1].as_str())
        .ok_or_else(|| DiscError::InvalidImage("command requires --output".into()))
}

fn main() -> Result<(), DiscError> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        usage();
        return Ok(());
    }
    let image = DiscImage::open(&args[2])?;
    match args[1].as_str() {
        "inspect" => {
            let info = image.boot_info()?;
            println!("Hardware: {}", info.hardware_id);
            println!("Fabricante: {}", info.maker_id);
            println!("Produto: {} {}", info.product_number, info.version);
            println!("Data: {}", info.release_date);
            println!("Dispositivo: {}", info.device_info);
            println!("Regioes: {}", info.compatible_areas);
            println!("Perifericos: {}", info.compatible_peripherals);
            println!("Titulo: {}", info.game_title);
            println!("IP.BIN: 0x{:X} bytes, carga em 0x06002000", info.ip_size);
            println!(
                "AIP: offset 0x{:X}, entrada em 0x{:08X}",
                info.aip_offset(),
                info.aip_address()
            );
            println!("STACK-M: 0x{:08X}", info.master_stack_address);
            println!("STACK-S: 0x{:08X}", info.slave_stack_address);
            println!(
                "FIRST READ ADDRESS (carga): 0x{:08X}",
                info.first_read_address
            );
            println!(
                "FIRST READ SIZE (ignorado em CD): 0x{:08X}",
                info.first_read_size
            );
            let first = image.first_read_file()?;
            let end = info
                .first_read_address
                .checked_add(first.size)
                .ok_or_else(|| {
                    DiscError::InvalidImage("first-read memory range overflow".into())
                })?;
            println!("Primeiro arquivo: {} ({} bytes)", first.path, first.size);
            println!(
                "Faixa carregada: 0x{:08X}..0x{:08X}",
                info.first_read_address, end
            );
            println!("Ponto de entrada: definido pelo AIP; nao presumido pelo leitor");
        }
        "list" => {
            let show_extents = args.iter().any(|argument| argument == "--extents");
            for entry in image.entries()? {
                let kind = if entry.is_directory { "DIR" } else { "FILE" };
                if show_extents {
                    println!("{kind}\t{}\t{}\t{}", entry.extent_lba, entry.size, entry.path);
                } else {
                    println!("{kind}\t{}\t{}", entry.size, entry.path);
                }
            }
        }
        "extract" => {
            let mut path = None;
            let mut output = None;
            let mut index = 3;
            while index < args.len() {
                if args[index] == "--output" && index + 1 < args.len() {
                    output = Some(args[index + 1].clone());
                    index += 2;
                } else {
                    path = Some(args[index].clone());
                    index += 1;
                }
            }
            let output = output
                .ok_or_else(|| DiscError::InvalidImage("extract requires --output".into()))?;
            let source = match path {
                Some(value) => value,
                None => image.first_read_file()?.path,
            };
            fs::write(output, image.extract_file(&source)?)?;
            println!("extracted {source}");
        }
        "extract-ip" => {
            let output = output_argument(&args[3..])?;
            let bytes = image.initial_program()?;
            fs::write(output, &bytes)?;
            println!("extracted IP.BIN ({} bytes)", bytes.len());
        }
        "extract-aip" => {
            let output = output_argument(&args[3..])?;
            let info = image.boot_info()?;
            let bytes = image.application_initial_program()?;
            fs::write(output, &bytes)?;
            println!(
                "extracted AIP ({} bytes, entry 0x{:08X})",
                bytes.len(),
                info.aip_address()
            );
        }
        _ => usage(),
    }
    Ok(())
}
