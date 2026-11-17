# Parse pkg-config's escaped arguments without CMake 3.16's legacy whitespace parser.
import os,shlex,subprocess,sys
source,output,module,bits=sys.argv[1:]
def query(arguments):
    process=subprocess.Popen(['pkg-config']+arguments+[module],stdout=subprocess.PIPE)
    value=process.communicate()[0]
    if process.returncode:raise SystemExit(process.returncode)
    if not isinstance(value,str):value=value.decode('utf-8')
    return value.strip()
flags=shlex.split(query(['--cflags','--libs']))
libdir=query(['--variable=libdir'])
command=[os.environ.get('CC','cc'),'-std=c99','-m'+bits,source,'-o',output,'-Wl,-R,'+libdir]+flags
raise SystemExit(subprocess.call(command))
