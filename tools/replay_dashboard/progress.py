"""Throttled CSV read progress shared by the terminal and browser."""
import csv
import gzip
import io
from pathlib import Path
import sys
import time


class TerminalProgress:
    def __init__(self, stream=None):
        self.stream = stream if stream is not None else sys.stdout
        self.last = 0
        self.key = None

    def __call__(self, state, force=False):
        now = time.monotonic()
        key = (state.get('caseId'), state.get('phase'), state.get('file'))
        if not force and key == self.key and now-self.last < 1:
            return
        self.last, self.key = now, key
        label = 'Preparing '+state['caseId'] if state.get('caseId') else 'Preparing'
        parts = ['['+label+']',state.get('phase','')]
        if state.get('casePercent') is not None:
            parts.append('case read %.1f%%'%state['casePercent'])
        if state.get('file'):
            parts.append(Path(state['file']).name)
        if state.get('filePercent') is not None:
            parts.append('file read %.1f%%'%state['filePercent'])
            parts.append('%.1f/%.1f MiB'%(state.get('fileBytesRead',0)/1048576,state.get('fileBytesTotal',0)/1048576))
        if state.get('rows'):
            parts.append('rows {:,}'.format(state['rows']))
        if state.get('samples'):
            parts.append('windows {:,}'.format(state['samples']))
        if state.get('simulationTime') is not None:
            parts.append('sim %.3fs'%state['simulationTime'])
        # Newline records work in a terminal, tee, log files and systemd.
        print(' | '.join(parts),file=self.stream,flush=True)


class Progress:
    def __init__(self, callback=None, terminal=None):
        self.callback = callback or (lambda state: None)
        self.terminal = terminal if terminal is not None else TerminalProgress()
        self.state = dict(status='processing',phase='checking',phaseLabel='检查输入与缓存',caseId='',file='',rows=0,filesDone=0)
        self.last = 0
        self.case_sizes = {}
        self.case_read = {}

    @staticmethod
    def resolve(path):
        path = Path(path)
        return Path(str(path)+'.gz') if not path.exists() and Path(str(path)+'.gz').exists() else path

    def case(self, case_id, paths):
        self.case_sizes = {str(self.resolve(p).resolve()):self.resolve(p).stat().st_size for p in paths if self.resolve(p).is_file()}
        self.case_read = {}
        # replay_run.csv was read to locate the optional audit directories.
        current = self.state.get('file')
        if current in self.case_sizes:
            self.case_read[current] = self.state.get('fileBytesRead',0)
        self.update(True,caseId=case_id)

    def update(self, force=False, **values):
        self.state.update(values)
        current = self.state.get('file')
        if current in self.case_sizes:
            self.case_read[current] = min(self.case_sizes[current],self.state.get('fileBytesRead',0))
        total = sum(self.case_sizes.values())
        done = sum(self.case_read.values())
        self.state.update(caseBytesRead=done,caseBytesTotal=total,casePercent=done/total*100 if total else None)
        now = time.monotonic()
        if force or now-self.last >= .25:
            self.callback(dict(self.state))
            self.terminal(self.state,force=force)
            self.last = now

    def stage(self, phase, label, case_id=''):
        self.case_sizes = {};self.case_read = {}
        self.update(True,phase=phase,phaseLabel=label,caseId=case_id,file='',rows=0,
                    fileBytesRead=0,fileBytesTotal=0,filePercent=None,samples=0,simulationTime=None)

    def rows(self, path):
        path = self.resolve(path).resolve()
        total = path.stat().st_size
        self.update(True,file=str(path),rows=0,fileBytesRead=0,fileBytesTotal=total,filePercent=0)
        with path.open('rb') as raw:
            binary = gzip.GzipFile(fileobj=raw) if path.suffix=='.gz' else raw
            with io.TextIOWrapper(binary,encoding='utf-8-sig',newline='') as stream:
                reader = csv.DictReader(stream)
                try:
                    for n,row in enumerate(reader,1):
                        self.state['rows'] = n
                        if n%2000==0:
                            done = min(total,raw.tell())
                            self.update(rows=n,fileBytesRead=done,filePercent=done/total*100 if total else 100)
                        yield row
                except Exception as error:
                    raise ValueError(f'{path}: CSV line {reader.line_num}: {error}') from error
                self.update(True,rows=max(0,reader.line_num-1),fileBytesRead=total,filePercent=100,
                            filesDone=self.state['filesDone']+1)

    def lines(self, path):
        """Native task traces, with the same byte-based progress as CSV input."""
        path=self.resolve(path).resolve();total=path.stat().st_size
        self.update(True,file=str(path),rows=0,fileBytesRead=0,fileBytesTotal=total,filePercent=0)
        with path.open('rb') as raw:
            binary=gzip.GzipFile(fileobj=raw) if path.suffix=='.gz' else raw
            with io.TextIOWrapper(binary,encoding='utf-8-sig') as stream:
                n=0
                for n,line in enumerate(stream,1):
                    if n%2000==0:
                        done=min(total,raw.tell());self.update(rows=n,fileBytesRead=done,filePercent=100*done/total if total else 100)
                    yield line
                self.update(True,rows=n,fileBytesRead=total,filePercent=100,filesDone=self.state['filesDone']+1)
