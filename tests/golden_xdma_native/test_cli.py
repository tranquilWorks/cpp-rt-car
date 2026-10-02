"""Actual host entry point; temporary missing/regular paths only, no live nodes."""
import pathlib, subprocess, sys, tempfile

def run(binary):
    with tempfile.TemporaryDirectory(prefix="native golden absent ") as directory:
        root=pathlib.Path(directory)
        rows={"tuple":"software-absence-test","bitstream_sha256":"a"*64,
              "driver_revision":"8721136e74a66500b02d16cb41922d966139cd46",
              "layout":"golden-xdma-native-v1","input0":"0","output0":"8192",
              "input1":"32768","output1":"40960"}
        for name in ("h2c0","h2c1","c2h0","c2h1","user","event0","event1"):
            rows[name]=str(root/name)
        config=root/"tuple.cfg"
        def invoke(expected):
            result=subprocess.run([binary,"--prepare",str(config)],capture_output=True,text=True,timeout=20)
            assert result.returncode==expected,(result.returncode,result.stdout,result.stderr)
            return result
        def save(): config.write_text("".join(f"{k}={v}\n" for k,v in rows.items()))
        save();result=invoke(3);assert "NOT_RUN missing device; no driver initialized" in result.stdout
        assert sorted(p.name for p in root.iterdir())==["tuple.cfg"]
        (root/"h2c0").write_text("ordinary file remains unchanged")
        invoke(2);assert (root/"h2c0").read_text()=="ordinary file remains unchanged"
        (root/"h2c0").unlink()
        rows["output0"]="0";save();invoke(2)
        rows["output0"]="8192";rows["unknown"]="1";save();invoke(2)
        del rows["unknown"];save();config.write_text(config.read_text()+"tuple=duplicate\n");invoke(2)
        save();del rows["event1"];save();invoke(2)
    print("PASS actual native CLI absence/configuration refusals; no live endpoint")
if __name__=="__main__":run(sys.argv[1])
