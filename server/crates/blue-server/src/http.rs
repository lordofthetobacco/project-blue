//! Minimal std-only HTTP/1.0 listener for `/healthz`, `/readyz` and
//! `/metrics`, on its own thread (never the tick thread).

use std::io::{self, Read as _, Write as _};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::Duration;

use crate::metrics::Metrics;

/// Response for a request line `GET <path> ...`: (status line, content
/// type, body).
#[must_use]
pub fn respond(request_line: &str, metrics: &Metrics) -> (&'static str, &'static str, String) {
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("");
    let path = parts.next().unwrap_or("");
    let path = path.split('?').next().unwrap_or("");
    if method != "GET" && method != "HEAD" {
        return (
            "405 Method Not Allowed",
            "text/plain",
            "method not allowed\n".to_owned(),
        );
    }
    match path {
        "/healthz" => ("200 OK", "text/plain", "ok\n".to_owned()),
        "/readyz" => {
            if metrics.ready.load(Ordering::Relaxed) {
                ("200 OK", "text/plain", "ready\n".to_owned())
            } else {
                (
                    "503 Service Unavailable",
                    "text/plain",
                    "not ready\n".to_owned(),
                )
            }
        }
        "/metrics" => ("200 OK", "text/plain; version=0.0.4", metrics.render()),
        _ => ("404 Not Found", "text/plain", "not found\n".to_owned()),
    }
}

fn serve(mut stream: TcpStream, metrics: &Metrics) -> io::Result<()> {
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    stream.set_write_timeout(Some(Duration::from_secs(2)))?;
    let mut buf = [0u8; 2048];
    let mut len = 0;
    while len < buf.len() {
        let n = stream.read(&mut buf[len..])?;
        if n == 0 {
            break;
        }
        len += n;
        if buf[..len].windows(4).any(|w| w == b"\r\n\r\n") || buf[..len].contains(&b'\n') {
            break;
        }
    }
    let text = String::from_utf8_lossy(&buf[..len]);
    let line = text.lines().next().unwrap_or("");
    let (status, content_type, body) = respond(line, metrics);
    let head_only = line.starts_with("HEAD ");
    write!(
        stream,
        "HTTP/1.0 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    if !head_only {
        stream.write_all(body.as_bytes())?;
    }
    stream.flush()
}

/// Binds `address` and serves until `stop` is set (checked every 50 ms).
///
/// # Errors
/// Bind errors.
pub fn spawn(
    address: &str,
    metrics: Arc<Metrics>,
    stop: Arc<AtomicBool>,
) -> io::Result<(JoinHandle<()>, std::net::SocketAddr)> {
    let listener = TcpListener::bind(address)?;
    listener.set_nonblocking(true)?;
    let local = listener.local_addr()?;
    let handle = std::thread::Builder::new()
        .name("blue-http".to_owned())
        .spawn(move || {
            while !stop.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        // One request per connection, served inline: these
                        // endpoints are cheap and scraped rarely.
                        let _ = serve(stream, &metrics);
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(50));
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(50)),
                }
            }
        })?;
    Ok((handle, local))
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]

    use super::*;

    #[test]
    fn routes() {
        let m = Metrics::new();
        assert_eq!(respond("GET /healthz HTTP/1.1", &m).0, "200 OK");
        assert_eq!(
            respond("GET /readyz HTTP/1.1", &m).0,
            "503 Service Unavailable"
        );
        m.ready.store(true, Ordering::Relaxed);
        assert_eq!(respond("GET /readyz HTTP/1.1", &m).0, "200 OK");
        let (status, kind, body) = respond("GET /metrics?x=1 HTTP/1.1", &m);
        assert_eq!(status, "200 OK");
        assert!(kind.starts_with("text/plain"));
        assert!(body.contains("blue_ready 1"));
        assert_eq!(respond("GET /nope HTTP/1.1", &m).0, "404 Not Found");
        assert_eq!(
            respond("POST /metrics HTTP/1.1", &m).0,
            "405 Method Not Allowed"
        );
        assert_eq!(respond("", &m).0, "405 Method Not Allowed");
    }

    #[test]
    fn serves_over_tcp() {
        let metrics = Arc::new(Metrics::new());
        let stop = Arc::new(AtomicBool::new(false));
        let (handle, addr) = spawn("127.0.0.1:0", metrics, Arc::clone(&stop)).unwrap();
        let mut stream = TcpStream::connect(addr).unwrap();
        stream
            .write_all(b"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")
            .unwrap();
        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();
        assert!(response.starts_with("HTTP/1.0 200 OK\r\n"));
        assert!(response.ends_with("\r\n\r\nok\n"));
        stop.store(true, Ordering::Relaxed);
        handle.join().unwrap();
    }
}
