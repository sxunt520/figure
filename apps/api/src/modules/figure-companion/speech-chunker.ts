const SENTENCE_ENDINGS = new Set(['。', '！', '？', '!', '?', '；', ';', '\n']);
const SOFT_BREAKS = new Set(['，', ',', '、', '：', ':']);

/**
 * Turns arbitrarily-sized LLM deltas into natural TTS chunks. Complete
 * sentences are emitted immediately. Long sentences may be split at a
 * natural pause so the first audio can start without waiting for the model.
 */
export class SpeechChunker {
  private buffer = '';
  private emittedAny = false;

  constructor(
    private readonly minimumSentenceLength = 5,
    private readonly firstSoftBreakLength = 8,
    private readonly softBreakLength = 22,
    private readonly hardBreakLength = 40,
  ) {}

  push(delta: string) {
    if (!delta) return [];
    this.buffer += delta;
    return this.takeReady(false);
  }

  flush() {
    return this.takeReady(true);
  }

  private takeReady(flush: boolean) {
    const chunks: string[] = [];
    let start = 0;
    let lastSoftBreak = -1;
    let bracketDepth = 0;

    for (let index = 0; index < this.buffer.length; index += 1) {
      const character = this.buffer[index];
      if ('（(【['.includes(character)) bracketDepth += 1;
      if ('）)】]'.includes(character) && bracketDepth > 0) bracketDepth -= 1;
      if (bracketDepth > 0) continue;

      if (SOFT_BREAKS.has(character)) lastSoftBreak = index;
      const length = index - start + 1;
      const preferredSoftBreakLength = this.emittedAny
        ? this.softBreakLength
        : this.firstSoftBreakLength;
      let end = -1;
      if (
        SENTENCE_ENDINGS.has(character) &&
        (flush || length >= this.minimumSentenceLength)
      ) {
        end = index + 1;
      } else if (
        SOFT_BREAKS.has(character) &&
        length >= preferredSoftBreakLength
      ) {
        end = index + 1;
      } else if (length >= this.hardBreakLength) {
        end = lastSoftBreak >= start ? lastSoftBreak + 1 : index + 1;
      }

      if (end <= start) continue;
      const chunk = this.buffer.slice(start, end).trim();
      if (chunk) {
        chunks.push(chunk);
        this.emittedAny = true;
      }
      start = end;
      lastSoftBreak = -1;
      index = start - 1;
    }

    this.buffer = this.buffer.slice(start);
    if (flush) {
      const tail = this.buffer.trim();
      if (tail) {
        chunks.push(tail);
        this.emittedAny = true;
      }
      this.buffer = '';
    }
    return chunks;
  }
}
