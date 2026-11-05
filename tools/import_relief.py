#!/usr/bin/env python3
"""Prepare the reference height grids and smooth hill art for native rendering.

No network access or source writes. Elevation changes visuals only. Coordinates,
step-2 grid, b-c split, slopes, holes and bell mounds follow web/gpu.js.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

SUN = (-1 / math.sqrt(3.21), 1.1 / math.sqrt(3.21), 1 / math.sqrt(3.21))


def shade(nx, ny, nz):
    length = math.sqrt(nx*nx + ny*ny + nz*nz) or 1
    return min(1.25, max(.55, 1 + .9 * ((nx*SUN[0]+ny*SUN[1]+nz*SUN[2])/length-SUN[1])))


def grid(info, payload):
    N = info['heights']
    bounds = info['bounds']
    if N < 4 or N > 1024 or len(payload) != N*N*2 or len(bounds) != 4 or not all(math.isfinite(v) for v in bounds):
        raise ValueError('Invalid elevation metadata/length')
    x0,x1,z0,z1 = bounds
    if x0 >= x1 or z0 >= z1 or max(map(abs,bounds)) > 2000000:
        raise ValueError('Invalid elevation bounds')
    samples = array('h'); samples.frombytes(payload)
    if sys.byteorder != 'little': samples.byteswap()
    if min(samples) < 0: raise ValueError('Elevation must be clamped at sea level')
    n = (N-1)//2+1
    h = [samples[j*2*N+i*2] for j in range(n) for i in range(n)]
    return dict(N=N,n=n,bounds=bounds,h=h)


def at(g, x, z):
    x0,x1,z0,z1 = g['bounds']; n=g['n']; N=g['N']
    gx=((x-x0)/(x1-x0)*N-.5)/2; gz=((z1-z)/(z1-z0)*N-.5)/2
    if not (0 <= gx <= n-1 and 0 <= gz <= n-1): return None
    i=min(int(gx),n-2);j=min(int(gz),n-2);u=gx-i;v=gz-j;h=g['h']
    a,b,c,d=h[j*n+i],h[j*n+i+1],h[(j+1)*n+i],h[(j+1)*n+i+1]
    return a+(b-a)*u+(c-a)*v if u+v<=1 else d+(c-d)*(1-u)+(b-d)*(1-v)


def geometry(g, grids, hills, hole):
    x0,x1,z0,z1=g['bounds'];N=g['N'];n=g['n'];h=g['h'];cell=(x1-x0)/N*2
    X=lambda i:x0+(i*2+.5)/N*(x1-x0)
    Z=lambda j:z1-(j*2+.5)/N*(z1-z0)
    vertices=array('f');indices=array('I')
    def put(x,y,z,s): vertices.extend((x,y,z,(x-x0)/(x1-x0),(z1-z)/(z1-z0),s))
    for j in range(n):
        for i in range(n):
            dx=(h[j*n+min(i+1,n-1)]-h[j*n+max(i-1,0)])/(2*cell)
            dz=(h[max(j-1,0)*n+i]-h[min(j+1,n-1)*n+i])/(2*cell)
            put(X(i),h[j*n+i]-(4 if hole else 0),Z(j),shade(-dx,1,-dz))
    def in_hole(i,j):
        return hole and hole[0]+cell<X(i)<hole[1]-cell and hole[2]+cell<Z(j)<hole[3]-cell
    for j in range(n-1):
        for i in range(n-1):
            if in_hole(i,j) and in_hole(i+1,j+1):continue
            a=j*n+i;b=a+1;c=a+n;d=c+1;indices.extend((a,c,b,b,c,d))
    def ground(x,z):
        for sample in grids:
            height=at(sample,x,z)
            if height is not None:return height
        return 0
    edge=math.exp(-(1/.35)**2)
    for hill in hills:
        values=[hill[k] for k in ('x','z','heading_deg','w','h','l')]
        if not all(math.isfinite(v) for v in values) or min(hill['w'],hill['h'],hill['l'])<=0:
            raise ValueError('Invalid hill')
        first=len(vertices)//6; start=len(indices);a=math.radians(hill['heading_deg']);cs=math.cos(a);sn=math.sin(a)
        for ring in range(15):
            r=ring/14;bell=.9*(math.exp(-(r/.35)**2)-edge)/(1-edge)
            for j in range(1 if ring==0 else 36):
                t=j/36*math.tau;lx=math.cos(t)*r*hill['w']/2*1.05;lz=math.sin(t)*r*hill['l']/2*1.05
                x=hill['x']+cs*lx-sn*lz;z=hill['z']+sn*lx+cs*lz
                put(x,ground(x,z)+hill['h']*bell-1,z,1)
        ix=lambda r,j:first+(0 if r==0 else 1+(r-1)*36+j%36)
        for j in range(36): indices.extend((ix(0,0),ix(1,j),ix(1,j+1)))
        for r in range(1,14):
            for j in range(36):indices.extend((ix(r,j),ix(r+1,j),ix(r+1,j+1),ix(r,j),ix(r+1,j+1),ix(r,j+1)))
        normals=[[0.,0.,0.] for _ in range(len(vertices)//6-first)]
        for k in range(start,len(indices),3):
            ids=indices[k:k+3];p=[vertices[i*6:i*6+3] for i in ids]
            u=[p[1][i]-p[0][i] for i in range(3)];v=[p[2][i]-p[0][i] for i in range(3)]
            normal=(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])
            for i in ids:
                for c in range(3):normals[i-first][c]+=normal[c]
        for i,normal in enumerate(normals):
            if normal[1]<0:normal=[-v for v in normal]
            vertices[(first+i)*6+5]=shade(*normal)
    heights=array('f',h)
    if sys.byteorder!='little':
        for a in (heights,vertices,indices):a.byteswap()
    return (b'LHXREL1\0'+struct.pack('<5I4d',N,2,n,len(vertices)//6,len(indices),*g['bounds'])+
            heights.tobytes()+vertices.tobytes()+indices.tobytes())


def convert(source,output):
    source,output=Path(source).resolve(),Path(output).resolve()
    if source==output or source in output.parents or output in source.parents:raise ValueError('Use a separate output directory')
    manifest={'format':'LHX relief 1','source':str(source),'visual_only':True,'theatres':{}}
    prepared=[]
    for name in ('gulf','asia','europe'):
        meta_path=source/f'{name}.json';meta=json.loads(meta_path.read_text());grids=[];entry={'attribution':meta['attribution'],'metadata_sha256':hashlib.sha256(meta_path.read_bytes()).hexdigest()}
        for label in ('near','far'):grids.append(grid(meta[label],(source/f'{name}_{label}.i16').read_bytes()))
        for i,label in enumerate(('near','far')):
            b=grids[0]['bounds'];hills=[h for h in meta.get('hills',[]) if (0 if b[0]<h['x']<b[1] and b[2]<h['z']<b[3] else 1)==i]
            if len(hills)>512:raise ValueError('Too many hills')
            payload=geometry(grids[i],grids,hills,b if i else None);filename=f'{name}_{label}.lhr';prepared.append((filename,payload))
            entry[label]={'file':filename,'sha256':hashlib.sha256(payload).hexdigest(),'source_sha256':hashlib.sha256((source/f'{name}_{label}.i16').read_bytes()).hexdigest(),'hills':len(hills)}
        manifest['theatres'][name]=entry
    output.mkdir(parents=True,exist_ok=True)
    for filename,payload in prepared:(output/filename).write_bytes(payload)
    (output/'RELIEF-SOURCE.json').write_text(json.dumps(manifest,indent=2)+'\n');return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,default=Path('bin/enhanced/terrain'));args=p.parse_args();convert(args.source,args.output);print('Imported relief for three theatres')
