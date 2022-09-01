/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-08-08
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
import CreateResource from './CreateResource'
import DataTable from './DataTable'
import MxsDlgs from './MxsDlgs'
import DurationDropdown from './DurationDropdown'
import DetailsPage from './DetailsPage'
import GlobalSearch from './GlobalSearch'
import IconSpriteSheet from './IconSpriteSheet'
import MonitorPageHeader from './MonitorPageHeader'
import MxsCharts from './MxsCharts'
import MxsCollapse from './MxsCollapse'
import MxsFilterList from './MxsFilterList'
import MxsSelect from './MxsSelect'
import MxsSplitPane from './MxsSplitPane'
import MxsSubMenu from './MxsSubMenu'
import MxsTreeview from './MxsTreeview'
import MxsTruncateStr from './MxsTruncateStr'
import MxsVirtualScrollTbl from './MxsVirtualScrollTbl'
import PageWrapper from './PageWrapper'
import Parameters from './Parameters'
import RepTooltip from './RepTooltip'
import RefreshRate from './RefreshRate'
import SessionsTable from './SessionsTable'
import OutlinedOverviewCard from './OutlinedOverviewCard'

//TODO: Rename other components to have mxs prefix
export default {
    'create-resource': CreateResource,
    'data-table': DataTable,
    'duration-dropdown': DurationDropdown,
    ...DetailsPage,
    'global-search': GlobalSearch,
    'icon-sprite-sheet': IconSpriteSheet,
    'monitor-page-header': MonitorPageHeader,
    ...MxsCharts,
    ...MxsDlgs,
    'mxs-collapse': MxsCollapse,
    'mxs-filter-list': MxsFilterList,
    'mxs-select': MxsSelect,
    'mxs-split-pane': MxsSplitPane,
    'mxs-sub-menu': MxsSubMenu,
    'mxs-treeview': MxsTreeview,
    'mxs-truncate-str': MxsTruncateStr,
    'mxs-virtual-scroll-tbl': MxsVirtualScrollTbl,
    'page-wrapper': PageWrapper,
    ...Parameters,
    'rep-tooltip': RepTooltip,
    'refresh-rate': RefreshRate,
    'sessions-table': SessionsTable,
    'outlined-overview-card': OutlinedOverviewCard,
}