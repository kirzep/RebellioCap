use std::{io::{self, BufRead, Write}, time::Duration};
fn main() {
    let mode = std::env::args().nth(1).unwrap();
    if mode == "incompatible" { println!(r#"{{"protocolVersion":2,"type":"ready"}}"#); return; }
    if mode == "malformed" { println!("broken"); return; }
    if mode == "oversized" { println!("{}", "x".repeat(65537)); return; }
    if mode == "startup_timeout" { std::thread::sleep(Duration::from_secs(60)); return; }
    println!(r#"{{"protocolVersion":1,"type":"ready","snapshot":{{"revision":1,"lifecycle":"ready","replayActive":true,"continuousRecordingActive":false,"replaySeconds":30,"metrics":{{"videoTicks":1,"missedVideoDeadlines":0,"videoPackets":1,"audioPackets":0,"saveRequests":0,"completedSaves":0,"failedSaves":0,"rejectedSaves":0,"pipelineErrors":0,"continuousPackets":0,"continuousFailures":0,"continuousRecordingActive":false,"lastHotkeySaveLatencyTicks":0,"replayBytes":0}}}}}}"#);
    io::stdout().flush().unwrap();
    if mode == "crash" { std::thread::sleep(Duration::from_millis(100)); std::process::exit(23); }
    if mode == "eof" { return; }
    if mode == "flood_timeout" {
        std::thread::spawn(|| loop {
            println!(r#"{{"protocolVersion":1,"type":"snapshot","snapshot":{{"revision":1,"lifecycle":"ready","replayActive":true,"continuousRecordingActive":false,"replaySeconds":30,"metrics":{{"videoTicks":1,"missedVideoDeadlines":0,"videoPackets":1,"audioPackets":0,"saveRequests":0,"completedSaves":0,"failedSaves":0,"rejectedSaves":0,"pipelineErrors":0,"continuousPackets":0,"continuousFailures":0,"continuousRecordingActive":false,"lastHotkeySaveLatencyTicks":0,"replayBytes":0}}}}}}"#);
            io::stdout().flush().unwrap();
            std::thread::sleep(Duration::from_millis(1));
        });
    }
    for line in io::stdin().lock().lines() {
        let line = line.unwrap();
        if mode == "timeout" || mode == "flood_timeout" { continue; }
        let id = line.split("\"requestId\":\"").nth(1).unwrap().split('"').next().unwrap();
        println!(r#"{{"protocolVersion":1,"type":"command_result","requestId":"{}"}}"#, id);
        if mode == "duplicate" { println!(r#"{{"protocolVersion":1,"type":"command_result","requestId":"{}"}}"#, id); }
        io::stdout().flush().unwrap();
        if line.contains("\"stop\"") { return; }
    }
}
