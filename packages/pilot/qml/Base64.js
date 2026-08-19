.pragma library

var alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

function utf8Bytes(text) {
    var encoded = encodeURIComponent(text)
    var bytes = []
    for (var index = 0; index < encoded.length; ++index) {
        if (encoded[index] === "%") {
            bytes.push(parseInt(encoded.substr(index + 1, 2), 16))
            index += 2
        } else {
            bytes.push(encoded.charCodeAt(index))
        }
    }
    return bytes
}

function encode(text) {
    var bytes = utf8Bytes(text)
    var output = ""
    for (var index = 0; index < bytes.length; index += 3) {
        var first = bytes[index]
        var second = index + 1 < bytes.length ? bytes[index + 1] : 0
        var third = index + 2 < bytes.length ? bytes[index + 2] : 0
        var block = (first << 16) | (second << 8) | third
        output += alphabet[(block >> 18) & 63]
        output += alphabet[(block >> 12) & 63]
        output += index + 1 < bytes.length ? alphabet[(block >> 6) & 63] : "="
        output += index + 2 < bytes.length ? alphabet[block & 63] : "="
    }
    return output
}

function decode(input) {
    if (typeof input !== "string" || input.length % 4 !== 0)
        throw new Error("Invalid base64")
    var bytes = []
    for (var index = 0; index < input.length; index += 4) {
        var first = alphabet.indexOf(input[index])
        var second = alphabet.indexOf(input[index + 1])
        var third = input[index + 2] === "=" ? 0 : alphabet.indexOf(input[index + 2])
        var fourth = input[index + 3] === "=" ? 0 : alphabet.indexOf(input[index + 3])
        if (first < 0 || second < 0 || third < 0 || fourth < 0)
            throw new Error("Invalid base64")
        var block = (first << 18) | (second << 12) | (third << 6) | fourth
        bytes.push((block >> 16) & 255)
        if (input[index + 2] !== "=") bytes.push((block >> 8) & 255)
        if (input[index + 3] !== "=") bytes.push(block & 255)
    }
    var percentEncoded = ""
    for (var byteIndex = 0; byteIndex < bytes.length; ++byteIndex)
        percentEncoded += "%" + bytes[byteIndex].toString(16).padStart(2, "0")
    return decodeURIComponent(percentEncoded)
}
