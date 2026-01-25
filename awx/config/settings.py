# AWX Settings Configuration
# https://docs.ansible.com/automation-controller/latest/html/administration/configure_controller.html

import os

# Database configuration (overridden by environment variables)
DATABASES = {
    'default': {
        'ATOMIC_REQUESTS': True,
        'ENGINE': 'django.db.backends.postgresql',
        'NAME': os.environ.get('DATABASE_NAME', 'awx'),
        'USER': os.environ.get('DATABASE_USER', 'awx'),
        'PASSWORD': os.environ.get('DATABASE_PASSWORD', 'awxpass'),
        'HOST': os.environ.get('DATABASE_HOST', 'awx-postgres'),
        'PORT': os.environ.get('DATABASE_PORT', '5432'),
        'OPTIONS': {
            'sslmode': os.environ.get('DATABASE_SSLMODE', 'prefer'),
        },
    }
}

# Redis / Caching
BROKER_URL = 'redis://{}:{}'.format(
    os.environ.get('REDIS_HOST', 'awx-redis'),
    os.environ.get('REDIS_PORT', '6379')
)

CHANNEL_LAYERS = {
    'default': {
        'BACKEND': 'channels_redis.core.RedisChannelLayer',
        'CONFIG': {
            'hosts': [BROKER_URL],
            'capacity': 10000,
            'expiry': 10,
        },
    },
}

CACHES = {
    'default': {
        'BACKEND': 'django_redis.cache.RedisCache',
        'LOCATION': '{}:/db/1'.format(BROKER_URL),
    },
}

# Cluster / HA settings
CLUSTER_HOST_ID = os.environ.get('HOSTNAME', 'awx')
AWX_AUTO_DEPROVISION_INSTANCES = True

# Session settings
SESSION_COOKIE_AGE = 1800  # 30 minutes
SESSION_COOKIE_SECURE = False  # Set to True if using HTTPS
CSRF_COOKIE_SECURE = False     # Set to True if using HTTPS

# Logging
LOGGING = {
    'version': 1,
    'disable_existing_loggers': False,
    'filters': {
        'require_debug_false': {
            '()': 'django.utils.log.RequireDebugFalse',
        },
    },
    'formatters': {
        'simple': {
            'format': '%(asctime)s %(levelname)-8s %(name)s %(message)s',
        },
    },
    'handlers': {
        'console': {
            'level': 'INFO',
            'class': 'logging.StreamHandler',
            'formatter': 'simple',
        },
    },
    'loggers': {
        'django': {
            'handlers': ['console'],
            'level': 'INFO',
        },
        'django.request': {
            'handlers': ['console'],
            'level': 'WARNING',
        },
        'awx': {
            'handlers': ['console'],
            'level': 'INFO',
        },
        'awx.main.scheduler': {
            'handlers': ['console'],
            'level': 'INFO',
        },
        'awx.main.tasks': {
            'handlers': ['console'],
            'level': 'INFO',
        },
    },
}

# Job settings
DEFAULT_JOB_TIMEOUT = 3600  # 1 hour
DEFAULT_INVENTORY_UPDATE_TIMEOUT = 600  # 10 minutes
DEFAULT_PROJECT_UPDATE_TIMEOUT = 300  # 5 minutes
MAX_FORKS = 200  # Max concurrent processes
AWX_PROOT_ENABLED = False  # Disable proot for better performance

# Callback / Notification settings
AWX_TASK_ENV = {
    'PROMETHEUS_PUSHGATEWAY': 'http://pushgateway:9091',
    'MAINTENANCE_WEBHOOK': os.environ.get('MAINTENANCE_WEBHOOK', ''),
}

# UI settings
TOWER_URL_BASE = os.environ.get('TOWER_URL_BASE', 'http://localhost:8052')
PENDO_TRACKING_STATE = 'off'
INSIGHTS_TRACKING_STATE = False

# Security
ALLOWED_HOSTS = ['*']  # Restrict in production!
REMOTE_HOST_HEADERS = ['HTTP_X_FORWARDED_FOR', 'REMOTE_ADDR', 'REMOTE_HOST']

# Activity stream settings (for audit trails)
ACTIVITY_STREAM_ENABLED = True
ACTIVITY_STREAM_ENABLED_FOR_INVENTORY_SYNC = True

# Scheduling limits
SCHEDULE_MAX_JOBS = 10  # Max concurrent scheduled jobs

# Project settings
AWX_ISOLATION_SHOW_PATHS = [
    '/var/lib/awx/projects/',
    '/tmp/',
    '/var/log/',
]

# Custom settings for Slurm maintenance
AWX_COLLECTIONS_PATH = '/var/lib/awx/vendor/awx/collections'
GALAXY_IGNORE_CERTS = False
