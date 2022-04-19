/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-04-08
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
import { APP_CONFIG } from 'utils/constants'
const Login = () => import(/* webpackChunkName: "login" */ 'pages/Login')
const NotFound404 = () => import(/* webpackChunkName: "not-found-page" */ 'pages/NotFound404')
//Dashboard views
const Dashboard = () => import(/* webpackChunkName: "dsh" */ 'pages/Dashboard')
const Servers = () => import(/* webpackChunkName: "dsh-servers" */ 'pages/Dashboard/Servers')
const Services = () => import(/* webpackChunkName: "dsh-services" */ 'pages/Dashboard/Services')
const Sessions = () => import(/* webpackChunkName: "dsh-sessions" */ 'pages/Dashboard/Sessions')
const Listeners = () => import(/* webpackChunkName: "dsh-listeners" */ 'pages/Dashboard/Listeners')
const Filters = () => import(/* webpackChunkName: "dsh-filters" */ 'pages/Dashboard/Filters')

//Visualization views
const Visualization = () => import(/* webpackChunkName: "vis" */ 'pages/Visualization')
const Conf = () => import(/* webpackChunkName: "vis-conf" */ 'pages/Visualization/Configuration')
const Clusters = () => import(/* webpackChunkName: "vis-clusters" */ 'pages/Visualization/Clusters')

//Query editor views
const QueryPage = () => import(/* webpackChunkName: "query-page" */ 'pages/QueryPage')
const QueryView = () => import(/* webpackChunkName: "query-view" */ 'pages/QueryPage/QueryView')

// Other views
const Users = () => import(/* webpackChunkName: "users" */ 'pages/Users')
const Logs = () => import(/* webpackChunkName: "logs" */ 'pages/Logs')
const Settings = () => import(/* webpackChunkName: "settings" */ 'pages/Settings')

// Detail views
const Server = () => import(/* webpackChunkName: "server-detail" */ 'pages/ServerDetail')
const Service = () => import(/* webpackChunkName: "service-detail" */ 'pages/ServiceDetail')
const Monitor = () => import(/* webpackChunkName: "monitor-detail" */ 'pages/MonitorDetail')
const Listener = () => import(/* webpackChunkName: "listener-detail" */ 'pages/ListenerDetail')
const Filter = () => import(/* webpackChunkName: "filter-detail" */ 'pages/FilterDetail')
const Cluster = () => import(/* webpackChunkName: "cluster-detail" */ 'pages/ClusterDetail')

const { DASHBOARD, VISUALIZATION, CLUSTER } = APP_CONFIG.ROUTE_GROUP
export const dashboardTabRoutes = [
    // Tab Routes
    {
        path: '/dashboard/servers',
        component: Servers,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: DASHBOARD,
        },
        name: 'servers',
        text: 'servers',
        isTabRoute: true,
    },
    {
        path: '/dashboard/sessions',
        component: Sessions,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: DASHBOARD,
        },
        name: 'sessions',
        text: 'current sessions',
        isTabRoute: true,
    },
    {
        path: '/dashboard/services',
        component: Services,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: DASHBOARD,
        },
        name: 'services',
        text: 'services',
        isTabRoute: true,
    },
    {
        path: '/dashboard/listeners',
        component: Listeners,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: DASHBOARD,
        },
        name: 'listeners',
        text: 'listeners',
        isTabRoute: true,
    },
    {
        path: '/dashboard/filters',
        component: Filters,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: DASHBOARD,
        },
        name: 'filters',
        text: 'filters',
        isTabRoute: true,
    },
]
export const visTabRoutes = [
    // Vis routes
    {
        path: '/visualization/configuration',
        component: Conf,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: VISUALIZATION,
        },
        name: 'configuration',
        text: 'configuration',
    },
    {
        path: '/visualization/clusters',
        component: Clusters,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: VISUALIZATION,
        },
        name: 'clusters',
        text: 'clusters',
    },
]
export const sideBarRoutes = [
    // Sidebar Routes
    {
        path: '/dashboard',
        component: Dashboard,
        meta: {
            requiresAuth: true,
            keepAlive: true,
            layout: 'app-layout',
            size: 22,
            icon: '$vuetify.icons.tachometer',
            redirect: '/dashboard/servers',
            group: DASHBOARD,
        },
        name: 'dashboard',
        label: 'dashboards',
        children: dashboardTabRoutes,
    },
    {
        path: '/visualization',
        component: Visualization,
        meta: {
            requiresAuth: true,
            keepAlive: true,
            layout: 'app-layout',
            size: 22,
            icon: '$vuetify.icons.reports',
            redirect: '/visualization/configuration',
            group: VISUALIZATION,
        },
        name: 'visualization',
        label: 'visualization',
        children: visTabRoutes,
    },
    {
        path: '/query',
        component: QueryPage,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            size: 22,
            icon: '$vuetify.icons.queryEditor',
        },
        name: 'queryEditor',
        label: 'queryEditor',
        children: [
            {
                path: ':type/:id',
                component: QueryView,
                meta: {
                    requiresAuth: true,
                    layout: 'app-layout',
                },
                name: 'query',
                props: route => ({
                    id: route.params.id,
                    type: route.params.type,
                }),
            },
        ],
    },
    {
        path: '/users',
        component: Users,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            size: 22,
            icon: '$vuetify.icons.users',
        },
        name: 'users',
        label: 'users',
    },
    {
        path: '/logs',
        component: Logs,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            size: 22,
            icon: '$vuetify.icons.logs',
        },
        name: 'logsArchive',
        label: 'logsArchive',
    },
    {
        path: '/settings',
        component: Settings,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            size: 22,
            icon: '$vuetify.icons.settings',
        },
        name: 'settings',
        label: 'settings',
    },
]

export const routes = [
    {
        path: '/',
        redirect: '/dashboard/servers',
    },
    {
        path: '/dashboard',
        redirect: '/dashboard/servers',
    },
    {
        path: '/dashboard/monitors',
        redirect: '/dashboard/servers',
    },
    {
        path: '/visualization',
        redirect: '/visualization/configuration',
    },
    {
        path: '*',
        redirect: '/404',
    },

    {
        path: '/login',
        name: 'login',
        component: Login,
        meta: {
            requiresAuth: false,
            guest: true,
            layout: 'no-layout',
        },
    },
    {
        path: '/404',
        name: 'not-found',
        component: NotFound404,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
        },
    },
    ...sideBarRoutes,
    // detail view routes
    {
        path: '/dashboard/services/:id',
        component: Service,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
        },
        name: 'service',
    },
    {
        path: '/dashboard/servers/:id',
        component: Server,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
        },
        name: 'server',
    },
    {
        path: '/dashboard/monitors/:id',
        component: Monitor,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
        },
        name: 'monitor',
    },
    {
        path: '/dashboard/listeners/:id',
        component: Listener,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
        },
        name: 'listener',
    },
    {
        path: '/dashboard/filters/:id',
        component: Filter,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
        },
        name: 'filter',
    },
    {
        path: '/visualization/clusters/:id',
        component: Cluster,
        meta: {
            requiresAuth: true,
            layout: 'app-layout',
            group: CLUSTER,
        },
        name: 'cluster',
    },
]
