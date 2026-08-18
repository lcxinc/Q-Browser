export type StrictJsonErrorCode =
  | "duplicate_json_member"
  | "invalid_json"
  | "invalid_utf8"
  | "json_too_complex"
  | "json_too_deep";

export class StrictJsonError extends Error {
  readonly code: StrictJsonErrorCode;

  constructor(code: StrictJsonErrorCode) {
    super(code);
    this.code = code;
  }
}

const MAX_DEPTH = 32;
const MAX_MEMBERS = 1_024;
const NUMBER = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;

class JsonPreflight {
  private readonly source: string;
  private index = 0;
  private members = 0;

  constructor(source: string) {
    this.source = source;
  }

  validate(): void {
    this.skipWhitespace();
    this.parseValue(0);
    this.skipWhitespace();
    if (this.index !== this.source.length) this.invalid();
  }

  private parseValue(depth: number): void {
    const current = this.source[this.index];
    if (current === "{") {
      this.parseObject(depth + 1);
      return;
    }
    if (current === "[") {
      this.parseArray(depth + 1);
      return;
    }
    if (current === '"') {
      this.parseString();
      return;
    }
    if (current === "t") {
      this.consumeLiteral("true");
      return;
    }
    if (current === "f") {
      this.consumeLiteral("false");
      return;
    }
    if (current === "n") {
      this.consumeLiteral("null");
      return;
    }
    if (current === "-" || (current !== undefined && current >= "0" && current <= "9")) {
      this.parseNumber();
      return;
    }
    this.invalid();
  }

  private parseObject(depth: number): void {
    this.checkDepth(depth);
    this.index += 1;
    this.skipWhitespace();
    if (this.consume("}")) return;

    const keys = new Set<string>();
    while (true) {
      if (this.source[this.index] !== '"') this.invalid();
      const key = this.parseString();
      if (keys.has(key)) throw new StrictJsonError("duplicate_json_member");
      keys.add(key);
      this.countMember();
      this.skipWhitespace();
      if (!this.consume(":")) this.invalid();
      this.skipWhitespace();
      this.parseValue(depth);
      this.skipWhitespace();
      if (this.consume("}")) return;
      if (!this.consume(",")) this.invalid();
      this.skipWhitespace();
    }
  }

  private parseArray(depth: number): void {
    this.checkDepth(depth);
    this.index += 1;
    this.skipWhitespace();
    if (this.consume("]")) return;

    while (true) {
      this.countMember();
      this.parseValue(depth);
      this.skipWhitespace();
      if (this.consume("]")) return;
      if (!this.consume(",")) this.invalid();
      this.skipWhitespace();
    }
  }

  private parseString(): string {
    if (!this.consume('"')) this.invalid();
    let decoded = "";
    while (this.index < this.source.length) {
      const codeUnit = this.source.charCodeAt(this.index++);
      if (codeUnit === 0x22) return decoded;
      if (codeUnit < 0x20) this.invalid();

      if (codeUnit === 0x5c) {
        const escape = this.source[this.index++];
        switch (escape) {
          case '"':
          case "/":
          case "\\":
            decoded += escape;
            break;
          case "b":
            decoded += "\b";
            break;
          case "f":
            decoded += "\f";
            break;
          case "n":
            decoded += "\n";
            break;
          case "r":
            decoded += "\r";
            break;
          case "t":
            decoded += "\t";
            break;
          case "u":
            decoded += this.parseUnicodeEscape();
            break;
          default:
            this.invalid();
        }
        continue;
      }

      if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
        const low = this.source.charCodeAt(this.index);
        if (low < 0xdc00 || low > 0xdfff) this.invalid();
        decoded += String.fromCharCode(codeUnit, low);
        this.index += 1;
        continue;
      }
      if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) this.invalid();
      decoded += String.fromCharCode(codeUnit);
    }
    this.invalid();
  }

  private parseUnicodeEscape(): string {
    const high = this.readHexCodeUnit();
    if (high >= 0xdc00 && high <= 0xdfff) this.invalid();
    if (high < 0xd800 || high > 0xdbff) return String.fromCharCode(high);

    if (this.source.slice(this.index, this.index + 2) !== "\\u") this.invalid();
    this.index += 2;
    const low = this.readHexCodeUnit();
    if (low < 0xdc00 || low > 0xdfff) this.invalid();
    return String.fromCharCode(high, low);
  }

  private readHexCodeUnit(): number {
    const digits = this.source.slice(this.index, this.index + 4);
    if (!/^[0-9a-fA-F]{4}$/.test(digits)) this.invalid();
    this.index += 4;
    return Number.parseInt(digits, 16);
  }

  private parseNumber(): void {
    NUMBER.lastIndex = this.index;
    const match = NUMBER.exec(this.source);
    if (match === null) this.invalid();
    this.index = NUMBER.lastIndex;
    if (!Number.isFinite(Number(match[0]))) this.invalid();
  }

  private consumeLiteral(literal: string): void {
    if (this.source.slice(this.index, this.index + literal.length) !== literal) {
      this.invalid();
    }
    this.index += literal.length;
  }

  private consume(expected: string): boolean {
    if (this.source[this.index] !== expected) return false;
    this.index += 1;
    return true;
  }

  private skipWhitespace(): void {
    while (
      this.source[this.index] === " " ||
      this.source[this.index] === "\t" ||
      this.source[this.index] === "\r" ||
      this.source[this.index] === "\n"
    ) {
      this.index += 1;
    }
  }

  private checkDepth(depth: number): void {
    if (depth > MAX_DEPTH) throw new StrictJsonError("json_too_deep");
  }

  private countMember(): void {
    this.members += 1;
    if (this.members > MAX_MEMBERS) {
      throw new StrictJsonError("json_too_complex");
    }
  }

  private invalid(): never {
    throw new StrictJsonError("invalid_json");
  }
}

export function parseStrictJson(bytes: Uint8Array): unknown {
  let source: string;
  try {
    source = new TextDecoder("utf-8", {
      fatal: true,
      ignoreBOM: true,
    }).decode(bytes);
  } catch {
    throw new StrictJsonError("invalid_utf8");
  }

  new JsonPreflight(source).validate();
  try {
    return JSON.parse(source) as unknown;
  } catch {
    throw new StrictJsonError("invalid_json");
  }
}
