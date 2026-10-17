using System.Security.Cryptography;
using System.Text;

namespace LegoBatmanOnline.Core;

/// <summary>
/// Schluesselpaar dieser Installation. Der Freundescode ist ein Hash des oeffentlichen Schluessels,
/// daher kann niemand einen fremden Code mit einem eigenen Schluessel belegen.
/// </summary>
public sealed class Identity : IDisposable
{
    private const string Alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // ohne I, O, 0, 1
    private readonly ECDiffieHellman _key;

    public byte[] PublicKey { get; }
    public string Id { get; }

    private Identity(ECDiffieHellman key)
    {
        _key = key;
        PublicKey = key.ExportSubjectPublicKeyInfo();
        Id = IdFromPublicKey(PublicKey);
    }

    public static Identity Create() => new(ECDiffieHellman.Create(ECCurve.NamedCurves.nistP256));

    public static Identity FromPrivateKey(byte[] pkcs8)
    {
        var k = ECDiffieHellman.Create();
        k.ImportPkcs8PrivateKey(pkcs8, out _);
        return new Identity(k);
    }

    public byte[] ExportPrivateKey() => _key.ExportPkcs8PrivateKey();

    /// <summary>12 Zeichen Base32 aus SHA-256 des oeffentlichen Schluessels (60 Bit).</summary>
    public static string IdFromPublicKey(byte[] spki)
    {
        var h = SHA256.HashData(spki);
        var sb = new StringBuilder();
        int bits = 0, acc = 0, i = 0;
        while (sb.Length < 12)
        {
            if (bits < 5) { acc = (acc << 8) | h[i++]; bits += 8; }
            sb.Append(Alphabet[(acc >> (bits - 5)) & 31]);
            bits -= 5;
        }
        return sb.ToString();
    }

    /// <summary>Anzeigeform: ABCD-EFGH-JKLM</summary>
    public static string FormatCode(string id) => $"{id[..4]}-{id[4..8]}-{id[8..12]}";

    /// <summary>Wandelt eine Eingabe wie "abcd efgh-jklm" in eine Id um, oder null.</summary>
    public static string? ParseCode(string input)
    {
        var sb = new StringBuilder();
        foreach (var c in input.ToUpperInvariant())
            if (Alphabet.Contains(c)) sb.Append(c);  // Bindestriche, Leerzeichen usw. ignorieren
        var s = sb.ToString();
        return s.Length == 12 ? s : null;
    }

    /// <summary>Gemeinsamer 256-Bit-Schluessel mit einem Partner (statisches ECDH).</summary>
    public byte[] SharedKey(byte[] peerSpki)
    {
        using var peer = ECDiffieHellman.Create();
        peer.ImportSubjectPublicKeyInfo(peerSpki, out _);
        return _key.DeriveKeyFromHash(peer.PublicKey, HashAlgorithmName.SHA256,
            Encoding.ASCII.GetBytes("lego-batman-online-v1"), null);
    }

    public void Dispose() => _key.Dispose();
}
