"""YOLOv4-tiny en numpy (poids reels Darknet) + simulation exacte de GEMM entier.
Convolution = im2col + GEMM, comme sur la carte. L'entier est emule en float64
(exact tant que |somme| < 2^53) ; la comparaison bit a bit avec le noyau NEON
est faite separement (cross_check_kernel)."""
import numpy as np, struct, re
from numpy.lib.stride_tricks import sliding_window_view

def parse_cfg(path):
    secs=[]; cur=None
    for ln in open(path):
        ln=ln.split('#')[0].strip()
        if not ln: continue
        if ln.startswith('['): cur={'type':ln.strip('[]')}; secs.append(cur)
        else:
            k,v=ln.split('=',1); cur[k.strip()]=v.strip()
    return secs

def load_net(cfg, weights):
    secs=parse_cfg(cfg); net=secs[0]; layers=secs[1:]
    d=open(weights,'rb').read(); off=20
    f=np.frombuffer(d,dtype='<f4',offset=off); p=0
    c_in=int(net.get('channels',3)); shapes=[]; 
    for i,L in enumerate(layers):
        t=L['type']
        if t=='convolutional':
            n=int(L['filters']); k=int(L['size']); bn=int(L.get('batch_normalize',0))
            bias=f[p:p+n].copy(); p+=n
            if bn:
                gamma=f[p:p+n].copy(); p+=n; mean=f[p:p+n].copy(); p+=n; var=f[p:p+n].copy(); p+=n
            W=f[p:p+n*c_in*k*k].reshape(n,c_in*k*k).copy(); p+=n*c_in*k*k
            if bn:
                s=gamma/np.sqrt(var+1e-5); W=W*s[:,None]; bias=bias-mean*s
            L.update(W=W.astype(np.float32),b=bias.astype(np.float32),n=n,k=k,c_in=c_in,
                     stride=int(L['stride']),leaky=(L.get('activation')=='leaky'))
            c_in=n
        elif t=='route':
            idx=[int(x) for x in L['layers'].split(',')]; idx=[j if j>=0 else i+j for j in idx]
            g=int(L.get('groups',1)); gid=int(L.get('group_id',0))
            L['idx']=idx; L['groups']=g; L['gid']=gid
            c=sum(shapes[j] for j in idx); c=c//g if g>1 else c
            c_in=c
        shapes.append(c_in)
    assert p==len(f), (p,len(f))
    return layers

def im2col(x,k,s):
    C,H,W=x.shape; pd=k//2
    xp=np.pad(x,((0,0),(pd,pd),(pd,pd)))
    w=sliding_window_view(xp,(k,k),axis=(1,2))[:,::s,::s]       # C,H',W',k,k
    Ho,Wo=w.shape[1],w.shape[2]
    return np.ascontiguousarray(w.transpose(0,3,4,1,2)).reshape(C*k*k,Ho*Wo),Ho,Wo

def qmax16(K): return int(np.floor(np.sqrt((2**31-1)/K)))

def quant_gemm(A,B,qc):
    """C ~ A@B avec quantification symetrique ; qc = dict(bits, per_channel, clip)."""
    K=A.shape[1]; qmax=qmax16(K) if qc['bits']==16 else 127
    A64=A.astype(np.float64); B64=B.astype(np.float64)
    if qc.get('qmax')=='data':      # qmax maximal sans debordement SUR CES DONNEES (garantie perdue)
        Cf=A.astype(np.float32)@B.astype(np.float32)
        r=np.abs(Cf).max()/(np.abs(A64).max()*np.abs(B64).max())
        qmax=int(min(32767,np.floor(0.9*np.sqrt((2**31-1)/r))))
    if qc.get('per_channel'):
        sA=qmax/np.maximum(np.abs(A64).max(axis=1),1e-30)           # une echelle par filtre
    else:
        sA=np.full(A.shape[0],qmax/max(np.abs(A64).max(),1e-30))
    qA=np.rint(A64*sA[:,None])
    if qc.get('clip'):
        sub=np.abs(B64.ravel()[::max(1,B64.size//2_000_000)])
        m=np.percentile(sub,qc['clip'])
    else: m=np.abs(B64).max()
    sB=qmax/max(m,1e-30)
    qB=np.clip(np.rint(B64*sB),-qmax,qmax)
    Ci=qA@qB                                                         # exact (|.|<2^53)
    ov=np.abs(Ci).max()
    assert ov<2**31, f"debordement int32 ({ov})"
    return (Ci/(sA[:,None]*sB)).astype(np.float32), qA.astype(np.int32), qB.astype(np.int32)

def forward(layers,x,qcfg=None,rec=None,trace=None):
    outs=[]; heads=[]; cur=x
    for i,L in enumerate(layers):
        t=L['type']
        if t=='convolutional':
            B,Ho,Wo=im2col(cur,L['k'],L['stride'])
            if qcfg is None: C=L['W']@B
            else: C=quant_gemm(L['W'],B,qcfg)[0]
            if rec is not None: rec[i]=(L['W'],B)
            C=C+L['b'][:,None]
            if L['leaky']: C=np.where(C>0,C,0.1*C)
            cur=C.reshape(L['n'],Ho,Wo).astype(np.float32)
        elif t=='route':
            ts=[outs[j] for j in L['idx']]
            if L['groups']>1:
                c=ts[0].shape[0]//L['groups']; ts=[ts[0][L['gid']*c:(L['gid']+1)*c]]
            cur=np.concatenate(ts,axis=0)
        elif t=='maxpool':
            C,H,W=cur.shape; cur=cur.reshape(C,H//2,2,W//2,2).max(axis=(2,4))
        elif t=='upsample':
            cur=cur.repeat(2,axis=1).repeat(2,axis=2)
        elif t=='yolo':
            heads.append((cur.copy(),L))
        outs.append(cur)
        if trace is not None: trace.append(cur.copy())
    return heads

def sigmoid(z): return 1/(1+np.exp(-z))

def decode(heads,net_w=416,thr=0.25,nms_iou=0.45):
    boxes=[]
    for out,L in heads:
        anchors=[float(a) for a in L['anchors'].replace(' ','').split(',')]
        anchors=np.array(anchors).reshape(-1,2); mask=[int(m) for m in L['mask'].split(',')]
        sxy=float(L.get('scale_x_y',1.0)); nc=int(L['classes'])
        C,H,W=out.shape; o=out.reshape(len(mask),5+nc,H,W)
        gx,gy=np.meshgrid(np.arange(W),np.arange(H))
        for a,m in enumerate(mask):
            tx,ty,tw,th,to=o[a,0],o[a,1],o[a,2],o[a,3],o[a,4]; cl=sigmoid(o[a,5:])
            bx=(sigmoid(tx)*sxy-0.5*(sxy-1)+gx)/W; by=(sigmoid(ty)*sxy-0.5*(sxy-1)+gy)/H
            bw=np.exp(tw)*anchors[m,0]/net_w; bh=np.exp(th)*anchors[m,1]/net_w
            score=sigmoid(to)[None]*cl                                 # nc,H,W
            ci,yi,xi=np.nonzero(score>thr)
            for c_,y_,x_ in zip(ci,yi,xi):
                boxes.append((score[c_,y_,x_],c_,bx[y_,x_]-bw[y_,x_]/2,by[y_,x_]-bh[y_,x_]/2,
                              bx[y_,x_]+bw[y_,x_]/2,by[y_,x_]+bh[y_,x_]/2))
    boxes=sorted(boxes,key=lambda t:-t[0])[:300]     # plafond : le NMS python est quadratique
    return nms(boxes,nms_iou)

def iou(a,b):
    ix=max(0,min(a[4],b[4])-max(a[2],b[2])); iy=max(0,min(a[5],b[5])-max(a[3],b[3])); i=ix*iy
    u=(a[4]-a[2])*(a[5]-a[3])+(b[4]-b[2])*(b[5]-b[3])-i
    return i/u if u>0 else 0

def nms(boxes,thr):
    keep=[]
    for b in sorted(boxes,key=lambda t:-t[0]):
        if all(not(k[1]==b[1] and iou(k,b)>thr) for k in keep): keep.append(b)
    return keep

def letterbox(img,size=416):
    from PIL import Image
    w,h=img.size; s=min(size/w,size/h); nw,nh=int(round(w*s)),int(round(h*s))
    im=img.convert('RGB').resize((nw,nh),Image.BILINEAR)
    can=Image.new('RGB',(size,size),(128,128,128)); can.paste(im,((size-nw)//2,(size-nh)//2))
    return np.asarray(can,dtype=np.float32).transpose(2,0,1)/255.0
