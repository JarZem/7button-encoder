#!/usr/bin/env python3
"""Generate OTA->ESP test vectors so OTA server can be completely bypassed.

Stage 1 prints a valid A frame for zigbee2mqtt/<device>/set.
Stage 2 verifies the R frame emitted by ESP and prints the encrypted P frame.
"""
import argparse, base64, hashlib, hmac, ipaddress, json, os, struct
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

KDF_DOMAIN=b"JaroslavZemanESP|provisioning-v1|"
NONCE_DOMAIN=b"JaroslavZemanESP|provisioning-nonce-v1|"
SECURITY={"OPEN":0,"WPA2":1,"WPA3":2,"WPA2_WPA3":3}

def b64u(b): return base64.urlsafe_b64encode(b).rstrip(b"=").decode()
def b64d(s): return base64.urlsafe_b64decode(s+"="*((-len(s))%4))
def rawsig(der):
    r,s=decode_dss_signature(der); return r.to_bytes(32,"big")+s.to_bytes(32,"big")
def dersig(raw): return encode_dss_signature(int.from_bytes(raw[:32],"big"),int.from_bytes(raw[32:],"big"))
def canon_a(device,counter,rnd): return f"A|{device}|{counter}|".encode()+rnd
def canon_r(device,counter,rnd): return f"R|{device}|{counter}|".encode()+rnd+b"|OK"
def load_key(p): return serialization.load_pem_private_key(Path(p).read_bytes(),password=None)
def load_cert(p): return x509.load_pem_x509_certificate(Path(p).read_bytes())
def derive(ota_key,device_cert,device,counter,rnd):
    shared=ota_key.exchange(ec.ECDH(),device_cert.public_key())
    return hmac.new(shared,KDF_DOMAIN+device.encode()+struct.pack(">Q",counter)+rnd,hashlib.sha256).digest()
def plain(ssid,password,host,port,security,channel):
    sb,pb=ssid.encode(),password.encode(); code=SECURITY[security.upper()]
    try:
        ip=ipaddress.ip_address(host); hb=ip.packed; ht=1
    except ValueError:
        hb=host.encode(); ht=0
    return bytes([1,code,channel,len(sb),len(pb),ht,len(hb)])+sb+pb+hb+struct.pack(">H",port)
def show(device,wire):
    print("TOPIC:",f"zigbee2mqtt/0x{device.replace(':','')}/set")
    print("PAYLOAD:",json.dumps({"ota_command":wire},separators=(",",":")))
    print("WIRE_BYTES:",len(wire.encode()))

def main():
    p=argparse.ArgumentParser(); sub=p.add_subparsers(dest="cmd",required=True)
    c=sub.add_parser("challenge"); c.add_argument("--device-id",required=True); c.add_argument("--counter",type=int,required=True); c.add_argument("--ota-key",required=True); c.add_argument("--device-cert",required=True); c.add_argument("--state",default="esp_target_session.json")
    q=sub.add_parser("provision"); q.add_argument("--state",default="esp_target_session.json"); q.add_argument("--response",required=True); q.add_argument("--ota-key",required=True); q.add_argument("--device-cert",required=True); q.add_argument("--ssid",required=True); q.add_argument("--password",required=True); q.add_argument("--host",required=True); q.add_argument("--port",type=int,default=8443); q.add_argument("--security",default="WPA2"); q.add_argument("--channel",type=int,default=0)
    d=sub.add_parser("ping"); d.add_argument("--device-id",required=True)
    a=p.parse_args()
    if a.cmd=="ping": show(a.device_id,"D|PING"); return
    if a.cmd=="challenge":
        ota=load_key(a.ota_key); dev=load_cert(a.device_cert); rnd=os.urandom(8)
        sig=rawsig(ota.sign(canon_a(a.device_id,a.counter,rnd),ec.ECDSA(hashes.SHA256())))
        wire=f"A|{b64u(rnd)}|{b64u(sig)}"; Path(a.state).write_text(json.dumps({"device_id":a.device_id,"counter":a.counter,"random":b64u(rnd)}))
        show(a.device_id,wire); print("NEXT: copy ESP R|... and run provision --response 'R|...'"); return
    st=json.loads(Path(a.state).read_text()); device=st["device_id"]; counter=int(st["counter"]); rnd=b64d(st["random"])
    dev=load_cert(a.device_cert); parts=a.response.split("|")
    if len(parts)!=2 or parts[0]!="R": raise SystemExit("invalid R frame")
    dev.public_key().verify(dersig(b64d(parts[1])),canon_r(device,counter,rnd),ec.ECDSA(hashes.SHA256()))
    print("R_VERIFY: OK")
    ota=load_key(a.ota_key); key=derive(ota,dev,device,counter,rnd)
    nonce=hmac.new(key,NONCE_DOMAIN+device.encode()+struct.pack(">Q",counter)+rnd,hashlib.sha256).digest()[:12]
    aad=f"P|{device}|{counter}|".encode()+rnd
    wire="P|"+b64u(AESGCM(key).encrypt(nonce,plain(a.ssid,a.password,a.host,a.port,a.security,a.channel),aad))
    if len(wire.encode())>100: raise SystemExit(f"P frame too long: {len(wire)}")
    show(device,wire)
if __name__=="__main__": main()
