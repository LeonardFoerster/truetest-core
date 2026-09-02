export async function readBoundedResponseText(
  response: Response,
  maxBytes: number,
): Promise<string> {
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 0)
    throw new RangeError("invalid_response_byte_limit");

  const contentLength = response.headers.get("content-length");
  if (contentLength !== null) {
    if (!/^(0|[1-9][0-9]*)$/.test(contentLength)) {
      void response.body?.cancel("invalid_results_content_length").catch(() => {});
      throw new TypeError("invalid_results_content_length");
    }
    const declaredBytes = Number(contentLength);
    if (!Number.isSafeInteger(declaredBytes) || declaredBytes > maxBytes) {
      void response.body?.cancel("results_report_too_large").catch(() => {});
      throw new RangeError("results_report_too_large");
    }
  }

  if (!response.body) throw new TypeError("missing_results_response_body");
  const reader = response.body.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  const chunks: string[] = [];
  const coalesceBytes = Math.min(maxBytes, 64 * 1024);
  const pending = new Uint8Array(coalesceBytes);
  let pendingBytes = 0;
  let receivedBytes = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.byteLength === 0) continue;
      receivedBytes += value.byteLength;
      if (receivedBytes > maxBytes) {
        throw new RangeError("results_report_too_large");
      }
      let offset = 0;
      while (offset < value.byteLength) {
        const copied = Math.min(pending.byteLength - pendingBytes,
          value.byteLength - offset);
        pending.set(value.subarray(offset, offset + copied), pendingBytes);
        pendingBytes += copied;
        offset += copied;
        if (pendingBytes === pending.byteLength) {
          chunks.push(decoder.decode(pending, { stream: true }));
          pendingBytes = 0;
        }
      }
    }
    if (pendingBytes > 0)
      chunks.push(decoder.decode(pending.subarray(0, pendingBytes), { stream: true }));
    chunks.push(decoder.decode());
    return chunks.join("");
  } catch (error) {
    // Cancellation is cleanup, never part of the financial validation
    // outcome. A hostile or broken stream must not be able to delay the
    // primary fail-closed rejection by returning a pending cancel promise.
    void reader.cancel("invalid_results_response").catch(() => {});
    throw error;
  } finally {
    reader.releaseLock();
  }
}
