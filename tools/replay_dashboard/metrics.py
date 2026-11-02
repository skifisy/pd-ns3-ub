"""Read externally computed experiment metrics, independently of replay SQLite."""
import csv
import math
from pathlib import Path

QUANTILES = ('p50', 'p90', 'p99', 'p999')
COUNTS = ('common_valid_ttft_requests', 'common_valid_tps_requests',
          'common_valid_throughput_requests', 'common_valid_completion_requests',
          'valid_fct_flows', 'common_valid_fct_flows')
VALUES = tuple(f'{metric}_{q}_ms' for metric in ('ttft', 'fct') for q in QUANTILES) + (
    'avg_tps', 'avg_throughput_gbps')
MISSING = ('', 'nan', 'na', 'n/a', 'none', 'null')


def metrics_path(manifest, config, override=None):
    configured = override or config.get('metrics_csv')
    if not configured:
        raise ValueError('Specify metrics_csv in the manifest or use --metrics-csv')
    return (Path(manifest).resolve().parent / configured).resolve()


def load_metrics(manifest, config, override=None):
    if config.get('statistics'):
        raise ValueError('statistics filters no longer apply in the dashboard; '
                         'apply them when generating metrics_csv and remove statistics from the manifest')
    path = metrics_path(manifest, config, override)
    schemes = {}
    with path.open(encoding='utf-8-sig', newline='') as stream:
        header = stream.readline()
        delimiter = '\t' if '\t' in header else ','
        stream.seek(0)
        reader = csv.DictReader(stream, delimiter=delimiter)
        fields = [field.strip() for field in (reader.fieldnames or [])]
        required = {'scheme', *COUNTS, *VALUES}
        if len(fields) != len(set(fields)) or not required <= set(fields):
            raise ValueError(f'{path}: duplicate columns or missing required columns: '
                             f'{", ".join(sorted(required - set(fields)))}')
        reader.fieldnames = fields
        for line, row in enumerate(reader, 2):
            if None in row or any(row[key] is None for key in required):
                raise ValueError(f'{path}:{line}: row does not match the CSV header')
            scheme = row['scheme'].strip().upper()
            if not scheme or scheme in schemes:
                raise ValueError(f'{path}:{line}: empty or duplicate scheme {scheme!r}')
            values = {'scheme': scheme}
            for key in COUNTS:
                try:
                    count = int(row[key].strip())
                except ValueError as error:
                    raise ValueError(f'{path}:{line}: {key} must be a nonnegative integer') from error
                if count < 0:
                    raise ValueError(f'{path}:{line}: {key} must be a nonnegative integer')
                values[key] = count
            if values['common_valid_fct_flows'] > values['valid_fct_flows']:
                raise ValueError(f'{path}:{line}: common FCT flow count exceeds valid flow count')
            for key in VALUES:
                raw = row[key].strip()
                if raw.lower() in MISSING:
                    values[key] = None
                    continue
                try:
                    value = float(raw)
                except ValueError as error:
                    raise ValueError(f'{path}:{line}: invalid metric {key}={raw!r}') from error
                if not math.isfinite(value) or value < 0:
                    raise ValueError(f'{path}:{line}: {key} must be finite and nonnegative')
                values[key] = value
            for prefix, count in (('ttft_', 'common_valid_ttft_requests'),
                                  ('fct_', 'common_valid_fct_flows'),
                                  ('avg_tps', 'common_valid_tps_requests'),
                                  ('avg_throughput_gbps', 'common_valid_throughput_requests')):
                if values[count] == 0:
                    for key in VALUES:
                        if key.startswith(prefix):
                            values[key] = None
            schemes[scheme] = values
    mapped = {}
    used = set()
    for case in config['cases']:
        scheme = str(case.get('metrics_scheme', case['id'])).strip().upper()
        if scheme not in schemes:
            raise ValueError(f'{path}: no metrics row for case {case["id"]!r}, scheme {scheme!r}')
        if scheme in used or case['id'] in mapped:
            raise ValueError(f'{path}: duplicate case or metrics_scheme mapping: {scheme}')
        used.add(scheme)
        mapped[case['id']] = schemes[scheme]
    return dict(source=path.name, cases=mapped,
                tpsUnit=str(config.get('metrics_tps_unit', 'TPS')))


def attach_metrics(bootstrap, metrics):
    """Overlay new CSV values on either a current or a legacy cached replay."""
    if {case['id'] for case in bootstrap['cases']} != set(metrics['cases']):
        raise ValueError('Cached replay case IDs differ from the manifest metrics mappings')
    obsolete = {'quantiles', 'p99', 'improvement', 'storageUtilization',
                'storageThroughputGbps', 'storageSampleComplete'}
    info = {key: value for key, value in bootstrap.items() if key not in ('commonTasks', 'statistics')}
    info['cases'] = [dict({key: value for key, value in case.items() if key not in obsolete},
                          metrics=dict(metrics['cases'][case['id']])) for case in bootstrap['cases']]
    info['metricsSource'] = metrics['source']
    info['metricsTpsUnit'] = metrics['tpsUnit']
    return info
