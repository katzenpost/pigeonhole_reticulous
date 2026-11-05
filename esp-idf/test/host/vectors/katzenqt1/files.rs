// Writes files.json from pigeonhole_protocols and reads back what another implementation wrote.
// A binary crate: Cargo.toml depends on pigeonhole_protocols (path to its rust/), serde_json, hex, sha2.
//   filevec gen <hushphone tests/data/tone.opus> > files.json
//   filevec read <json>   (test_client writes the C++ engine's boxes there when KQ_FILES_OUT is set)
use pigeonhole_protocols::protocols::katzenqt1::wire::{self, FileUpload};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

const BOX: usize = 1549;

fn bytes_of(n: usize, seed: usize) -> Vec<u8> {
    (0..n).map(|i| ((i * 31 + seed) % 251) as u8).collect()
}

fn vector(name: &str, version: u64, payload: Vec<u8>, basename: &str) -> Value {
    let mh = vec![0x11u8; 32];
    let read_cap: Vec<u8> = (0..136).map(|i| i as u8).collect();
    let f = FileUpload { payload: Vec::new(), filetype: wire::filetype_of(basename).into(), basename: basename.into() };
    let content = wire::file_content(&mh, &f, version);
    let message = wire::put_payload(&content, &payload).expect("put");
    let total = wire::chunk_count(message.len(), BOX);
    let chunks: Vec<String> = (0..total).map(|i| hex::encode(wire::substream_box(&message, BOX, i))).collect();
    json!({
        "name": name, "version": version, "membership_hash": hex::encode(&mh), "filetype": f.filetype,
        "basename": basename, "payload": hex::encode(&payload), "content": hex::encode(&content),
        "message": hex::encode(&message), "box": hex::encode(wire::frame(&message)), "box_size": BOX,
        "fits": message.len() + 1 <= BOX, "chunks": chunks, "read_cap": hex::encode(&read_cap),
        "indirection": hex::encode(wire::indirection(total, &read_cap)),
    })
}

fn generate(tone: &str) {
    let clip = std::fs::read(tone).expect("tone.opus");
    let opus = format!("{}.opus", hex::encode(Sha256::digest(&clip)));
    // The payload length whose message fills exactly two chunk boxes.
    let mh = vec![0x11u8; 32];
    let f = FileUpload { payload: Vec::new(), filetype: "image/png".into(), basename: "edge.png".into() };
    let over = wire::file_content(&mh, &f, 4).len() + 2;  // a 2-byte length header grows to 3 here
    let edge = 2 * (BOX - 1) - over;
    assert_eq!(wire::put_payload(&wire::file_content(&mh, &f, 4), &vec![0; edge]).unwrap().len(), 2 * (BOX - 1));
    let v = vec![
        vector("hushphone tone.opus", 4, clip, &opus),
        vector("inline png", 4, bytes_of(500, 7), "a.png"),
        vector("empty file", 4, Vec::new(), "nothing"),
        vector("two chunks exactly", 4, bytes_of(edge, 3), "edge.png"),
        vector("image in 13 chunks", 4, bytes_of(20000, 7), "cat.png"),
        vector("map form, version 3", 3, bytes_of(3000, 5), "doc.bin"),
    ];
    println!("{}", serde_json::to_string_pretty(&json!({
        "about": "katzenqt1 files as pigeonhole_protocols writes them (filevec gen, test/host/vectors/katzenqt1/files.rs)",
        "vectors": v})).unwrap());
}

fn read(path: &str) {
    let doc: Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    let (mut ok, mut bad) = (0, 0);
    for v in doc["vectors"].as_array().unwrap() {
        let h = |k: &str| hex::decode(v[k].as_str().unwrap()).unwrap();
        let chunks: Vec<Vec<u8>> = v["chunks"].as_array().unwrap().iter().map(|c| hex::decode(c.as_str().unwrap()).unwrap()).collect();
        let message = if chunks.is_empty() {
            match wire::unframe(&h("box")) { wire::Chunk::Final(m) => Some(m), _ => None }
        } else {
            wire::assemble(&chunks)
        };
        let name = v["name"].as_str().unwrap();
        let got = message.as_ref().and_then(|m| {
            let (without, payload) = wire::take_payload(m)?;
            wire::message_from_cbor(&without)?;
            let f = wire::file_of(&wire::Gcm::from_bytes(m)?)?;
            Some((f, payload))
        });
        let pass = match &got {
            Some((f, payload)) => *payload == h("payload") && f.payload == h("payload")
                && f.filetype == v["filetype"].as_str().unwrap() && f.basename == v["basename"].as_str().unwrap(),
            None => false,
        };
        if v.get("indirection").is_some() && !chunks.is_empty() {
            if let wire::Chunk::Indirection { total, read_cap } = wire::unframe(&h("indirection")) {
                if total != Some(chunks.len() as u32) || read_cap != h("read_cap") { bad += 1; println!("FAIL {name}: I box"); continue; }
            } else { bad += 1; println!("FAIL {name}: not an I box"); continue; }
        }
        if pass { ok += 1; println!("ok   {name}: {} chunks, file of {} bytes", chunks.len(), h("payload").len()); }
        else { bad += 1; println!("FAIL {name}"); }
    }
    println!("filevec read: {ok} read, {bad} failed");
    if bad > 0 { std::process::exit(1); }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    match a.get(1).map(String::as_str) {
        Some("gen") => generate(&a[2]),
        Some("read") => read(&a[2]),
        _ => eprintln!("filevec gen <tone.opus> | read <json>"),
    }
}
